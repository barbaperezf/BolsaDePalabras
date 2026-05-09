/**
 * ============================================================
 *  Bolsa de Palabras -- Version PARALELA (MPI)
 * ============================================================
 *
 *  Compilacion:
 *    mpicxx -O2 -std=c++17 BdP_mpi.cpp -lcurl -o BdP_mpi
 *
 *  Ejecucion:
 *   mpiexec -n 4 BdP_MPI.exe
 *   mpirun -np 4 ./BdP_mpi urls.txt
 *
 *  Salida:
 *    bdp_mpi.csv     -- matriz Bolsa de Palabras
 *    speedup.txt     -- tiempos y speed-up
 * ============================================================
 */

#include <mpi.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cctype>
#include <curl/curl.h>

using namespace std;

const string ARCHIVO_SALIDA = "bdp_mpi.csv";

// ---------------------------------------------
//  Callback para libcurl: acumula el texto descargado
// ---------------------------------------------
size_t curlCallback(void* data, size_t size, size_t nmemb, string* out) {
    out->append((char*)data, size * nmemb);
    return size * nmemb;
}

// ---------------------------------------------
//  Descarga el contenido de una URL
//  Retorna "[ERROR]" si falla
// ---------------------------------------------
string descargarLibro(const string& url) {
    CURL* curl = curl_easy_init();      //inicializar libcurl
    if (!curl) return "[ERROR] No se pudo descargar el libro.";
 
    string respuesta;
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &respuesta);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
 
    CURLcode ok = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
 
    if (ok == CURLE_OK)
        return respuesta;
    else
        return "[ERROR] No se pudo descargar el libro.";
}

// ---------------------------------------------
//  Extrae el titulo del libro desde las primeras
//  Si no encuentra el campo, devuelve el fallback.
// ---------------------------------------------
string extraerTitulo(const string& raw, const string& fallback) {
    istringstream iss(raw); // tratar el string como un flujo de lineas
    string linea;
 
    for (int i = 0; i < 60 && getline(iss, linea); i++) {    // Solo revisamos las primeras 60 lineas (cabecera Gutenberg)
        if (linea.rfind("Title:", 0) == 0) {          // empieza con "Title:"
            string titulo = linea.substr(6);           // quitar "Title:"
            // Quitar espacios al inicio y \r al final
            while (!titulo.empty() && (titulo.front() == ' '))
                titulo.erase(titulo.begin());
            while (!titulo.empty() && (titulo.back() == '\r' || titulo.back() == ' '))
                titulo.pop_back();
            if (titulo.empty()) 
                return fallback;
            else
                return titulo;
        }
    }
    return fallback;
}

// ---------------------------------------------
//  Recorta el encabezado/pie de Project Gutenberg
//  Conserva solo el contenido literario entre
//  "*** START OF" y "*** END OF"
// ---------------------------------------------
string recortarGutenberg(const string& raw) {
    size_t inicio = raw.find("*** START OF");
    size_t fin    = raw.find("*** END OF");
 
    if (inicio == string::npos) return raw;   // sin marcador: devolver todo
 
    // Saltar hasta la siguiente linea despues del marcador de inicio
    size_t contenido = raw.find('\n', inicio);
    if (contenido == string::npos) return raw;
    contenido++;
 
    if (fin == string::npos || fin <= contenido)
        return raw.substr(contenido);
 
    return raw.substr(contenido, fin - contenido);
}

// ---------------------------------------------
//  Limpia el texto:
//  - Convierte a minusculas
//  - Reemplaza todo lo que no sea letra por espacio
// ---------------------------------------------
string limpiarTexto(const string& texto) {
    string limpio;
    limpio.reserve(texto.size());   //para evitar reallocs innecesarios
    for (unsigned char c : texto) {    // usar unsigned char para evitar problemas con caracteres >127
        if (isalpha(c))
            limpio += (char)tolower(c);
        else
            limpio += ' ';
    }
    return limpio;
}

// ---------------------------------------------
//  Tokeniza el texto limpio y cuenta frecuencias.
//  Retorna mapa: palabra → numero de apariciones
// ---------------------------------------------
map<string, int> contarPalabras(const string& texto) {
    map<string, int> freq;  //las nuevas palabras las iniicaliza con 0, luego se incrementan
    istringstream iss(texto);
    string token;
    while (iss >> token) 
        freq[token]++;
    return freq;
}

// ---------------------------------------------
//  Lee URLs desde un archivo (una por linea)
// ---------------------------------------------
vector<string> leerURLs(const string& archivo) {
    vector<string> urls;
    ifstream f(archivo);    //abrir el archivo
    if (!f.is_open()) {
        cerr << "[ERROR] No se pudo abrir: " << archivo << "\n";
        return urls;
    }
    string linea;
    while (getline(f, linea)) {
        // Limpiar \r y espacios finales
        while (!linea.empty() && (linea.back() == '\r' || linea.back() == ' '))
            linea.pop_back();
        if (!linea.empty())
            urls.push_back(linea);
    }
    return urls;
}

// ---------------------------------------------
//  Fallback de nombre desde URL
//  ".../pg1342.txt" --> "pg1342"
// ---------------------------------------------
string nombreDesdeURL(const string& url) {
    size_t barra = url.rfind('/');
    string nombre;
    if (barra != string::npos)
        nombre = url.substr(barra + 1);
    else
        nombre = url;
    size_t punto = nombre.rfind('.');
    if (punto != string::npos) nombre = nombre.substr(0, punto);
    return nombre;
}

// -----------------------------------------------------------
//  MAIN MPI
// -----------------------------------------------------------
int main(int argc, char* argv[]) {

    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Acepta el archivo de URLs como argumento opcional.
    // Si no se pasa ninguno, usa "urls.txt" por defecto.
    // Permite correr tanto con:
    //   mpiexec -n 4 BdP_MPI.exe
    //   mpirun -np 4 ./BdP_mpi urls.txt
    string archivo_urls;
    if (argc >=2)
        archivo_urls = argv[1];
    else
        archivo_urls = "urls.txt";

    double t_inicio = MPI_Wtime();

    // ------------------------------------------
    //  FASE 1: Proceso 0 lee y broadcast de URLs
    // ------------------------------------------
    int k = 0;
    string urls_buf;       // todas las URLs en un solo string separadas por '\n'
    vector<string> urls;

    if (rank == 0) {
        urls = leerURLs(archivo_urls);
        k    = urls.size();
        for (const string& u : urls) { urls_buf += u; urls_buf += '\n'; }

        cout << "-----------------------------------------------------\n";
        cout << "  Bolsa de Palabras -- Version MPI\n";
        cout << "  Procesos: " << size << " | Libros: " << k << "\n";
        cout << "-----------------------------------------------------\n\n";
    }

    // Broadcast: numero de libros y buffer de URLs
    MPI_Bcast(
        &k,        //enviamos el numero de libros para que los demas procesos sepan cuanto espacio reservar para las URLs
        1,         //cantidad de elementos a enviar
        MPI_INT, 
        0,         //lo envía el proceso 0
        MPI_COMM_WORLD
    );
    // en MPI no puedes mandar un string direcamente con MPI_Bcast. 
    // primero tenemos que enviar el tamaño, que los receptores recerven memoria y luego ya le envias los datos reales del string.
    int buf_len = urls_buf.size();
    MPI_Bcast(&buf_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank != 0) urls_buf.resize(buf_len);
    MPI_Bcast(&urls_buf[0], buf_len, MPI_CHAR, 0, MPI_COMM_WORLD);

    // Todos los procesos deserializan las URLs
    if (rank != 0) {
        istringstream iss(urls_buf);
        string linea;
        while (getline(iss, linea))
            if (!linea.empty()) urls.push_back(linea);
    }

    // ------------------------------------------
    //  FASE 2: Cada proceso selecciona sus libros
    //  Distribucion ciclica: libro i → proceso (i % size)
    // ------------------------------------------
    vector<int> mis_libros;
    for (int i = 0; i < k; i++)
        if (i % size == rank) mis_libros.push_back(i); //push_back para agregar un elemento al final del vector

    // ------------------------------------------
    //  FASE 3: Trabajo local en paralelo
    //  Cada proceso descarga y procesa sus libros
    // ------------------------------------------
    // titulos_local[i] y freq_local[i] → para libro global mis_libros[i]
    int n_local = mis_libros.size();
    vector<string>           titulos_local(n_local);
    vector<map<string, int>> freq_local(n_local);

    for (int j = 0; j < n_local; j++) {
        int i = mis_libros[j];
        cout << "[Rank " << rank << "] Descargando libro " << i
             << ": " << urls[i] << "\n";

        string crudo = descargarLibro(urls[i]);
        if (crudo.empty()) {
            titulos_local[j] = nombreDesdeURL(urls[i]);
            continue;
        }

        titulos_local[j]  = extraerTitulo(crudo, nombreDesdeURL(urls[i]));
        string contenido  = recortarGutenberg(crudo);
        string limpio     = limpiarTexto(contenido);
        freq_local[j]     = contarPalabras(limpio);

        cout << "  Titulo: " << titulos_local[j]
             << "  | Palabras unicas: " << freq_local[j].size() << "\n";
    }

    // ------------------------------------------
    //  FASE 4: Construir vocabulario global
    //
    //  Estrategia:
    //  a) Cada proceso serializa sus palabras locales → string "w1|w2|w3|"
    //  b) MPI_Gather de tamanos
    //  c) MPI_Gatherv de los strings
    //  d) Proceso 0 fusiona, ordena y serializa el vocab global con indices
    //  e) MPI_Bcast del vocab global
    //  f) Todos deserializan
    // ------------------------------------------

    // a) Serializar palabras locales
    string vocab_local_str;
    for (int j = 0; j < n_local; j++)
        for (auto& [palabra, _] : freq_local[j]) {
            vocab_local_str += palabra;
            vocab_local_str += '|';
        }

    // b) Gather de tamanos
    int local_len = vocab_local_str.size();
    vector<int> todos_lens(size);
    MPI_Gather( 
        &local_len,         //buffer a enviar
        1, 
        MPI_INT, 
        todos_lens.data(),  //buffer donde se recopilan los datos
        1, 
        MPI_INT, 
        0,                  //proceso que recopila los datos 
        MPI_COMM_WORLD
    );

    // c) Gatherv de strings de vocabulario
    vector<int> desplazamientos(size, 0);
    int total_bytes = 0;
    if (rank == 0) {
        for (int r = 0; r < size; r++) {
            desplazamientos[r] = total_bytes;
            total_bytes += todos_lens[r];
        }
    }

    string vocab_todos_str;
    if (rank == 0) vocab_todos_str.resize(total_bytes);

    MPI_Gatherv(        //gather con tamaños variables
        vocab_local_str.data(), local_len, MPI_CHAR,    //qué envía cada proceso
        rank == 0 ? &vocab_todos_str[0] : nullptr,      //donde se recopilan los datos (si soy el proceso 0 paso el buffer receptor, si no paso nullptr)
        todos_lens.data(), desplazamientos.data(), MPI_CHAR,    //tamaños y desplazamientos para recolectar los datos
        0, MPI_COMM_WORLD
    );

    // d) Proceso 0: fusionar con set (deduplicacion y orden automatico)
    string vocab_global_str;
    int V = 0;

    if (rank == 0) {
        set<string> vocab_set;
        istringstream iss(vocab_todos_str);
        string palabra;
        while (getline(iss, palabra, '|'))
            if (!palabra.empty()) vocab_set.insert(palabra);

        // Serializar: "palabra:indice|palabra:indice|..."
        int idx = 0;
        for (const string& p : vocab_set) {
            vocab_global_str += p + ":" + to_string(idx++) + "|";  //cada palabra seguida de su indice, separadas por ':', y cada par separado por '|'
        }
        V = idx;
        cout << "\nVocabulario global: " << V << " palabras unicas.\n\n";
    }

    // e) Broadcast del vocab global
    // igual, primero mando el tamaño para que los demás procesos reserven memoria, luego mando el string con las palabras e indices
    int vg_len = vocab_global_str.size();
    MPI_Bcast(&vg_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank != 0) vocab_global_str.resize(vg_len);
    MPI_Bcast(&vocab_global_str[0], vg_len, MPI_CHAR, 0, MPI_COMM_WORLD);

    // f) Todos deserializan el vocabulario global
    //    Resultado: vocabulario (vector ordenado) e indice (map palabra→col)
    vector<string> vocabulario;
    map<string, int> indice;
    { //esto es para limitar el alcance de las variables temporales usadas en la deserializacion
        istringstream iss(vocab_global_str);
        string entrada;
        while (getline(iss, entrada, '|')) {
            if (entrada.empty()) continue;
            size_t dos_puntos = entrada.rfind(':');
            string pal = entrada.substr(0, dos_puntos);
            int    idx = stoi(entrada.substr(dos_puntos + 1)); //stoi = string to int
            // Asegurar tamano del vector
            if ((int)vocabulario.size() <= idx) vocabulario.resize(idx + 1);
            vocabulario[idx] = pal;
            indice[pal]      = idx;
        }
    }
    V = vocabulario.size();

    // ------------------------------------------
    //  FASE 5: Cada proceso construye sus filas BoW
    // ------------------------------------------
    // Buffer plano: [n_local filas * V columnas], todas inicializadas en 0. Luego se llenan las frecuencias locales.
    vector<int> filas_locales(n_local * V, 0);

    for (int j = 0; j < n_local; j++)
        for (auto& [palabra, freq] : freq_local[j])
            if (indice.count(palabra))  //retorna 1 si la palabra existe en el vocabulario global, 0 si no
                filas_locales[j * V + indice[palabra]] = freq;  //fila j, columna indice[palabra], esa sintaxis es para "aplanar" la matriz en un vector. posición = fila * número_de_columnas + columna

    // ------------------------------------------
    //  FASE 6: Recolectar todas las filas en proceso 0
    // ------------------------------------------
    int envio_count = n_local * V;
    vector<int> todos_counts(size);
    MPI_Gather(
        &envio_count, 1, MPI_INT,           //qué cantidad de datos envío (n_local filas * V columnas)
        todos_counts.data(), 1, MPI_INT,    //donde se recopilan los tamaños 
        0,                                  //quién lo recolecta
        MPI_COMM_WORLD);

    vector<int> displs_filas(size, 0);
    int total_ints = 0;
    if (rank == 0) {
        for (int r = 0; r < size; r++) {
            displs_filas[r] = total_ints;
            total_ints += todos_counts[r];
        }
    }

    vector<int> filas_recibidas;
    if (rank == 0) filas_recibidas.resize(total_ints);

    MPI_Gatherv(
        filas_locales.data(), envio_count, MPI_INT,
        rank == 0 ? filas_recibidas.data() : nullptr,
        todos_counts.data(), displs_filas.data(), MPI_INT,
        0, MPI_COMM_WORLD
    );

    // ------------------------------------------
    //  FASE 7: Recolectar titulos en proceso 0
    //  Mismo patron: Gather de tamanos + Gatherv de strings
    // ------------------------------------------

    // Serializar titulos locales: "titulo0\ntitulo1\n..."
    string titulos_local_str;
    for (const string& t : titulos_local) { titulos_local_str += t; titulos_local_str += '\n'; }

    int tit_local_len = titulos_local_str.size();
    vector<int> tit_lens(size);
    MPI_Gather(&tit_local_len, 1, MPI_INT, tit_lens.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

    vector<int> tit_displs(size, 0);
    int tit_total = 0;
    if (rank == 0) {
        for (int r = 0; r < size; r++) { tit_displs[r] = tit_total; tit_total += tit_lens[r]; }
    }

    string titulos_todos_str;
    if (rank == 0) titulos_todos_str.resize(tit_total);

    MPI_Gatherv(
        titulos_local_str.data(), tit_local_len, MPI_CHAR,
        rank == 0 ? &titulos_todos_str[0] : nullptr,
        tit_lens.data(), tit_displs.data(), MPI_CHAR,
        0, MPI_COMM_WORLD
    );

    double t_fin = MPI_Wtime();
    double t_paralelo = t_fin - t_inicio;

    // ------------------------------------------
    //  FASE 8: Proceso 0 reordena y escribe CSV
    // ------------------------------------------
    if (rank == 0) {

        // -- Reconstruir titulos en orden libro 0..k-1 --
        // Los titulos llegaron agrupados por proceso: [proc0][proc1]...
        // proc r tiene libros: r, r+size, r+2*size, ...
        // dentro de cada bloque el orden es j=0,1,2,...

        // Primero parsear todos los titulos recibidos por proceso
        // tit_displs[r] marca donde empieza el bloque del proceso r
        vector<string> titulos_finales(k);

        for (int r = 0; r < size; r++) {
            // Extraer el substring del proceso r
            string bloque = titulos_todos_str.substr(
                tit_displs[r],
                (r + 1 < size) ? (tit_displs[r+1] - tit_displs[r])
                               : (tit_total - tit_displs[r])
            );
            istringstream iss(bloque);
            string tit;
            int j = 0;
            while (getline(iss, tit)) {
                if (tit.empty()) continue;
                int libro_global = r + j * size;   // distribucion ciclica
                if (libro_global < k)
                    titulos_finales[libro_global] = tit;
                j++;
            }
        }

        // -- Reconstruir matriz en orden libro 0..k-1 --
        vector<vector<int>> matriz(k, vector<int>(V, 0));

        for (int r = 0; r < size; r++) {
            int offset = displs_filas[r];
            int j = 0;
            for (int i = r; i < k; i += size) {       // libros del proceso r
                for (int col = 0; col < V; col++)
                    matriz[i][col] = filas_recibidas[offset + j * V + col];
                j++;
            }
        }

        // -- Escribir CSV ------------------------------
        cout << "Escribiendo " << ARCHIVO_SALIDA << "...\n";
        {
            ofstream csv(ARCHIVO_SALIDA);
            // Encabezado
            csv << "titulo";
            for (const string& p : vocabulario) csv << "," << p;
            csv << "\n";
            // Filas
            for (int i = 0; i < k; i++) {
                csv << titulos_finales[i];
                for (int j = 0; j < V; j++) csv << "," << matriz[i][j];
                csv << "\n";
            }
        }

        // -- Speed-up ---------------------------------
        double t_serial = -1.0;
        ifstream tf("tiempo_serial.txt");
        if (tf.is_open()) tf >> t_serial;
        double speedup = (t_serial > 0.0) ? t_serial / t_paralelo : -1.0;

        cout << "\n-----------------------------------------------------\n";
        cout << "  RESULTADOS -- Version MPI\n";
        cout << "-----------------------------------------------------\n";
        cout << "  Procesos          : " << size        << "\n";
        cout << "  Libros procesados : " << k           << "\n";
        cout << "  Vocabulario global: " << V           << " palabras\n";
        cout << "  Matriz generada   : " << ARCHIVO_SALIDA << "\n";
        cout << "  Tiempo paralelo   : " << t_paralelo  << " s\n";
        if (speedup > 0.0) {
            cout << "  Tiempo serial     : " << t_serial  << " s\n";
            cout << "  Speed-up          : " << speedup   << "x\n";
        }
        cout << "-----------------------------------------------------\n";

    }

    MPI_Finalize();
    return 0;
}