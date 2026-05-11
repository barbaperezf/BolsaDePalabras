// Bolsa de Palabras -- Version Paralela con MPI
// Cómputo Paralelo y en la Nube - Proyecto de Clausura
// Fernando Barba y Nicolas Robles
//
// Compilacion: mpicxx -std=c++17 -O2 BdP_MPI.cpp -lcurl -o BdP_MPI
// Ejecucion:   mpirun -np 4 ./BdP_MPI urls.txt


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


// Se usa libcurl para descargar los libros y guarda el texto en un buffer de strings
size_t curlCallback(void* data, size_t size, size_t nmemb, string* out) {
    out->append((char*)data, size * nmemb);
    return size * nmemb;
}


// Descarga el contenido de una URL y lo devuelve como string
// Si algo falla devuelve "" para que el caller lo detecte con .empty() y se salte el libro
string descargarLibro(const string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    string respuesta;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &respuesta);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode ok = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (ok == CURLE_OK) return respuesta;
    return "";
}


// Busca el campo "Title:" en las primeras 60 lineas de la cabecera de Gutenberg
string extraerTitulo(const string& raw, const string& fallback) {
    istringstream iss(raw);
    string linea;

    // Se limita a buscar el titulo en las primeras 60 lineas para asegurar que es el titulo
    for (int i = 0; i < 60 && getline(iss, linea); i++) {
        if (linea.rfind("Title:", 0) == 0) {
            string titulo = linea.substr(6);
            while (!titulo.empty() && titulo.front() == ' ')
                titulo.erase(titulo.begin());
            while (!titulo.empty() && (titulo.back() == '\r' || titulo.back() == ' '))
                titulo.pop_back();
            if (titulo.empty()) return fallback;
            return titulo;
        }
    }
    return fallback;
}


// Recorta el encabezado y pie de Gutenberg conservando solo el contenido literario entre "*** START OF" y "*** END OF"
// Si no se recortara, los conteos se ensucian con el texto legal y los prefacios
string recortarGutenberg(const string& raw) {
    size_t inicio = raw.find("*** START OF");
    size_t fin    = raw.find("*** END OF");

    // Verficar que encontro el inicio antes de buscar el contenido
    if (inicio == string::npos) return raw;

    size_t contenido = raw.find('\n', inicio);
    if (contenido == string::npos) return raw;
    contenido++;

    // Verificar que encontro el fin y que esta despues del contenido
    if (fin == string::npos || fin <= contenido)
        return raw.substr(contenido);

    return raw.substr(contenido, fin - contenido);
}


// Pasa el texto a minusculas y reemplaza con espacio todo lo que no sea letra
string limpiarTexto(const string& texto) {
    string limpio;
    limpio.reserve(texto.size());
    for (unsigned char c : texto) {     // unsigned char para evitar UB en isalpha con caracteres >127
        if (isalpha(c)) limpio += (char)tolower(c);
        else            limpio += ' ';
    }
    return limpio;
}


// El texto limpio se se pasa a un mapa palabra -> frecuencia contando cuantas veces aparece cada palabra en el texto
map<string, int> contarPalabras(const string& texto) {
    map<string, int> freq;
    istringstream iss(texto);
    string token;
    while (iss >> token) freq[token]++;
    return freq;
}


// Lee las URLs del archivo de entrada, una por linea y las devuelve en un vector<string>
vector<string> leerURLs(const string& archivo) {
    vector<string> urls;
    ifstream f(archivo);
    if (!f.is_open()) {
        cerr << "[ERROR] No se pudo abrir: " << archivo << "\n";
        return urls;
    }
    string linea;
    while (getline(f, linea)) {
        while (!linea.empty() && (linea.back() == '\r' || linea.back() == ' '))
            linea.pop_back();
        if (!linea.empty()) urls.push_back(linea);
    }
    return urls;
}

// Metodo auxiliar para obtener un nombre de libro desde la URL
// Ejemplo: ".../pg1342.txt" --> "pg1342"
string nombreDesdeURL(const string& url) {
    size_t barra = url.rfind('/');
    string nombre = (barra != string::npos) ? url.substr(barra + 1) : url;
    size_t punto = nombre.rfind('.');
    if (punto != string::npos) nombre = nombre.substr(0, punto);
    return nombre;
}


// Escapa un campo para CSV (RFC 4180), necesario para titulos con coma como "Moby Dick; Or, The Whale"
string escaparCSV(const string& s) {
    if (s.find(',')  == string::npos &&
        s.find('"')  == string::npos &&
        s.find('\n') == string::npos)
        return s;

    string r = "\"";
    for (char c : s) {
        if (c == '"') r += "\"\"";
        else          r += c;
    }
    r += "\"";
    return r;
}


// MPI lanza q copias del mismo programa al mismo tiempo, cada una con su propio rank (0..q-1)
// Los procesos no comparten memoria, se comunican por paso de mensajes (Bcast, Gather, Gatherv)
int main(int argc, char* argv[]) {

    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);   // id del proceso actual
    MPI_Comm_size(MPI_COMM_WORLD, &size);   // q, numero total de procesos

    string archivo_urls = (argc >= 2) ? argv[1] : "urls.txt";

    // Cronometro arranca despues de MPI_Init pero antes de leer URLs, igual que en el serial para comparar de forma justa
    double t_inicio = MPI_Wtime();


    // Paso 1: Rank 0 lee las URLs y se las pasa a los demas via broadcast
    int k = 0;
    string urls_buf;
    vector<string> urls;

    if (rank == 0) {
        urls = leerURLs(archivo_urls);
        k    = (int)urls.size();
        // Se serializan las URLs en un solo string para mandarlas con un solo Bcast
        for (const string& u : urls) { urls_buf += u; urls_buf += '\n'; }

        cout << "-----------------------------------------------------\n";
        cout << "  Bolsa de Palabras -- Version MPI\n";
        cout << "  Procesos: " << size << " | Libros: " << k << "\n";
        cout << "-----------------------------------------------------\n\n";
    }

    // En MPI no se manda un string directo: primero el tamaño para que los receptores reserven memoria, luego los bytes
    // Es decir, se hace un broadcast de urls_buf.size() y luego de urls_buf.data()
    MPI_Bcast(&k, 1, MPI_INT, 0, MPI_COMM_WORLD);
    int buf_len = (int)urls_buf.size();
    MPI_Bcast(&buf_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank != 0) urls_buf.resize(buf_len);
    MPI_Bcast(&urls_buf[0], buf_len, MPI_CHAR, 0, MPI_COMM_WORLD);

    // Solo el primer proceso lee el archivo y llena urls_buf, los demas reciben el buffer con Bcast y lo parsean para llenar su vector urls localmente
    if (rank != 0) {
        istringstream iss(urls_buf);
        string linea;
        while (getline(iss, linea))
            if (!linea.empty()) urls.push_back(linea);
    }


    // Paso 2: Cada proceso elige sus libros con distribucion ciclica
    vector<int> mis_libros;
    for (int i = 0; i < k; i++)
        if (i % size == rank) mis_libros.push_back(i);


    // Paso 3: Cada proceso descarga y procesa sus libros en paralelo
    // Aqui es donde ocurre el trabajo paralelo y de donde sale el speed-up. 
    int n_local = (int)mis_libros.size();
    vector<string>           titulos_local(n_local);
    vector<map<string, int>> freq_local(n_local);

    // Cada proceso descarga y procesa sus libros asignados, llenando titulos_local y freq_local
    // No hay comunicacion entre procesos en esta fase, cada quien hace su trabajo con sus libros
    for (int j = 0; j < n_local; j++) {
        int i = mis_libros[j];
        cout << "[Rank " << rank << "] Descargando libro " << i << ": " << urls[i] << "\n";

        string crudo = descargarLibro(urls[i]);
        if (crudo.empty()) {
            titulos_local[j] = nombreDesdeURL(urls[i]);
            continue;
        }

        titulos_local[j] = extraerTitulo(crudo, nombreDesdeURL(urls[i]));
        string contenido = recortarGutenberg(crudo);
        string limpio    = limpiarTexto(contenido);
        freq_local[j]    = contarPalabras(limpio);

        cout << "  Titulo: " << titulos_local[j] << " | Palabras unicas: " << freq_local[j].size() << "\n";
    }


    // Paso 4: Construir el vocabulario global combinando los vocabularios locales de cada proceso
    // Cada proceso serializa sus palabras locales
    string vocab_local_str;
    for (int j = 0; j < n_local; j++)
        for (auto& [palabra, _] : freq_local[j]) {
            vocab_local_str += palabra;
            vocab_local_str += '|';
        }

    // Gather de tamaños: cada proceso le dice al rank 0 cuantos bytes manda
    int local_len = (int)vocab_local_str.size();
    vector<int> todos_lens(size);
    MPI_Gather(&local_len, 1, MPI_INT, todos_lens.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

    // Gatherv del contenido (gather con tamaños variables porque cada proceso manda distinta cantidad)
    vector<int> desplazamientos(size, 0);
    int total_bytes = 0;
    if (rank == 0) {
        for (int r = 0; r < size; r++) {
            desplazamientos[r] = total_bytes;
            total_bytes += todos_lens[r];
        }
    }

    // El rank 0 recibe todas las palabras locales concatenadas en un solo string
    string vocab_todos_str;
    if (rank == 0) vocab_todos_str.resize(total_bytes);

    MPI_Gatherv(
        vocab_local_str.data(), local_len, MPI_CHAR,
        rank == 0 ? &vocab_todos_str[0] : nullptr,
        todos_lens.data(), desplazamientos.data(), MPI_CHAR,
        0, MPI_COMM_WORLD
    );

    // El rank 0 reconstruye el vocabulario global a partir de las palabras locales recibidas de todos los procesos
    string vocab_global_str;
    int V = 0;
    if (rank == 0) {
        set<string> vocab_set;
        istringstream iss(vocab_todos_str);
        string palabra;
        while (getline(iss, palabra, '|'))
            if (!palabra.empty()) vocab_set.insert(palabra);

        // Para cada palabra del vocabulario global se asigna un indice de columna, y se serializa como "palabra:indice|"
        int idx = 0;
        for (const string& p : vocab_set)
            vocab_global_str += p + ":" + to_string(idx++) + "|";
        V = idx;
        cout << "\nVocabulario global: " << V << " palabras unicas.\n\n";
    }

    // Broadcast del vocabulario global a todos los procesos
    int vg_len = (int)vocab_global_str.size();
    MPI_Bcast(&vg_len, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank != 0) vocab_global_str.resize(vg_len);
    MPI_Bcast(&vocab_global_str[0], vg_len, MPI_CHAR, 0, MPI_COMM_WORLD);

    // Todos deserializan: reconstruyen el vector y el mapa palabra -> indice de columna
    vector<string> vocabulario;
    map<string, int> indice;
    {
        istringstream iss(vocab_global_str);
        string entrada;

        // Para cada entrada "palabra:indice" se llena el vocabulario global y el mapa de indice
        while (getline(iss, entrada, '|')) {
            if (entrada.empty()) continue;
            size_t dos_puntos = entrada.rfind(':');
            string pal = entrada.substr(0, dos_puntos);
            int    idx = stoi(entrada.substr(dos_puntos + 1));
            if ((int)vocabulario.size() <= idx) vocabulario.resize(idx + 1);
            vocabulario[idx] = pal;
            indice[pal]      = idx;
        }
    }
    V = (int)vocabulario.size();


    // Paso 5: Cada proceso construye sus filas locales de la matriz BoW en un buffer 
    // Cada proceso llena un vector<int> con sus filas locales, con la misma cantidad de columnas que el vocabulario global, 
    // y con los conteos de cada palabra local en la columna correspondiente del vocabulario global
    vector<int> filas_locales(n_local * V, 0);

    for (int j = 0; j < n_local; j++)
        for (auto& [palabra, freq] : freq_local[j])
            if (indice.count(palabra))
                filas_locales[j * V + indice[palabra]] = freq;     // posicion = fila * num_columnas + columna


    // Paso 6: Recolectar todas las filas locales en el rank 0 con Gatherv
    int envio_count = n_local * V;
    vector<int> todos_counts(size);
    MPI_Gather(&envio_count, 1, MPI_INT, todos_counts.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

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


    // Paso 7: Recolectar los titulos en el rank 0 para escribir el CSV con Gatherv 
    string titulos_local_str;
    for (const string& t : titulos_local) {
        titulos_local_str += t;
        titulos_local_str += '\n';
    }

    int tit_local_len = (int)titulos_local_str.size();
    vector<int> tit_lens(size);
    MPI_Gather(&tit_local_len, 1, MPI_INT, tit_lens.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

    vector<int> tit_displs(size, 0);
    int tit_total = 0;
    if (rank == 0) {
        for (int r = 0; r < size; r++) {
            tit_displs[r] = tit_total;
            tit_total += tit_lens[r];
        }
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


    // Paso 8: Solo el rank 0 reordena los datos al orden libro 0..k-1 y escribe el CSV
    if (rank == 0) {

        vector<string> titulos_finales(k);
        for (int r = 0; r < size; r++) {
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
                int libro_global = r + j * size;   
                if (libro_global < k)
                    titulos_finales[libro_global] = tit;
                j++;
            }
        }

        // Se crea la matriz final con los datos recibidos de los otros procesos
        vector<vector<int>> matriz(k, vector<int>(V, 0));
        for (int r = 0; r < size; r++) {
            int offset = displs_filas[r];
            int j = 0;
            for (int i = r; i < k; i += size) {
                for (int col = 0; col < V; col++)
                    matriz[i][col] = filas_recibidas[offset + j * V + col];
                j++;
            }
        }

        cout << "Escribiendo " << ARCHIVO_SALIDA << "...\n";
        {
            ofstream csv(ARCHIVO_SALIDA);
            csv << "titulo";
            for (const string& p : vocabulario) csv << "," << p;
            csv << "\n";
            for (int i = 0; i < k; i++) {
                csv << escaparCSV(titulos_finales[i]);
                for (int j = 0; j < V; j++) csv << "," << matriz[i][j];
                csv << "\n";
            }
        }

        // Speed-up: lee el tiempo serial del archivo que dejo el programa serial al terminar su ejecucion
        double t_serial = -1.0;
        ifstream tf("tiempo_serial.txt");
        if (tf.is_open()) tf >> t_serial;
        double speedup = (t_serial > 0.0) ? t_serial / t_paralelo : -1.0;

        cout << "\n-----------------------------------------------------\n";
        cout << "  RESULTADOS -- Version MPI\n";
        cout << "-----------------------------------------------------\n";
        cout << "  Procesos          : " << size       << "\n";
        cout << "  Libros procesados : " << k          << "\n";
        cout << "  Vocabulario global: " << V          << " palabras\n";
        cout << "  Matriz generada   : " << ARCHIVO_SALIDA << "\n";
        cout << "  Tiempo paralelo   : " << t_paralelo << " s\n";
        if (speedup > 0.0) {
            cout << "  Tiempo serial     : " << t_serial << " s\n";
            cout << "  Speed-up          : " << speedup  << "x\n";
        } else {
            cout << "  (Para ver el speed-up, corre primero BdP_serial)\n";
        }
        cout << "-----------------------------------------------------\n";
    }

    MPI_Finalize();
    return 0;
}
