// Bolsa de Palabras -- Version Serial
// Cómputo Paralelo y en la Nube - Proyecto de Clausura
// Fernando Barba y Nicolas Robles
//
// Compilacion: g++ -std=c++17 -O2 BdP_serial.cpp -lcurl -o BdP_serial
// Ejecucion:   ./BdP_serial urls.txt


#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <curl/curl.h>

using namespace std;

const string ARCHIVO_SALIDA = "bdp_serial.csv";


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
// Si no lo encuentra, devuelve el fallback (nombre desde URL)
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


// Metodo auxiliar para que si un titulo tiene coma, salto de linea o comillas, 
// se escapen con comillas dobles y se dupliquen las comillas internas, siguiendo el formato CSV
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


int main(int argc, char* argv[]) {

    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <urls.txt>\n";
        return 1;
    }

    // Cronometro arranca antes de leer URLs para medir las mismas fases que el MPI y comparar el speed-up de forma justa
    auto t_inicio = chrono::high_resolution_clock::now();

    // Paso 1: Leer la lista de URLs
    vector<string> urls = leerURLs(argv[1]);
    int k = (int)urls.size();
    if (k == 0) {
        cerr << "[ERROR] El archivo de URLs esta vacio.\n";
        return 1;
    }

    cout << "-----------------------------------------------------\n";
    cout << "  Bolsa de Palabras -- Version Serial\n";
    cout << "  Libros a procesar: " << k << "\n";
    cout << "-----------------------------------------------------\n\n";

    vector<string>           titulos(k);
    vector<map<string, int>> frecuencias(k);

    // Paso 2: Descargar y procesar cada libro uno a uno para poder mapearlo
    for (int i = 0; i < k; i++) {
        cout << "[" << (i+1) << "/" << k << "] " << urls[i] << "\n";

        string crudo = descargarLibro(urls[i]);
        if (crudo.empty()) {
            cerr << "  [WARN] No se pudo descargar, se omite.\n";
            titulos[i] = nombreDesdeURL(urls[i]);
            continue;
        }

        // El titulo se extrae antes de recortar la cabecera porque "Title:" vive en la cabecera
        titulos[i] = extraerTitulo(crudo, nombreDesdeURL(urls[i]));

        string contenido = recortarGutenberg(crudo);
        string limpio    = limpiarTexto(contenido);
        frecuencias[i]   = contarPalabras(limpio);

        cout << "  Titulo  : " << titulos[i] << "\n";
        cout << "  Palabras unicas: " << frecuencias[i].size() << "\n\n";
    }

    // Paso 3: Construir el vocabulario global
    // Se usa set<string> porque inserta sin duplicados y queda ordenado alfabeticamente
    cout << "Construyendo vocabulario global...\n";

    set<string> vocab_set;
    for (int i = 0; i < k; i++)
        for (auto& [palabra, _] : frecuencias[i])
            vocab_set.insert(palabra);

    vector<string> vocabulario(vocab_set.begin(), vocab_set.end());
    int V = (int)vocabulario.size();

    // Mapea que palabra corresponde a que indice de columna
    map<string, int> indice;
    for (int j = 0; j < V; j++)
        indice[vocabulario[j]] = j;

    cout << "Vocabulario global: " << V << " palabras unicas.\n\n";

    // Paso 4: Llenar la matriz de libros x palabras con los conteos de cada libro
    cout << "Construyendo matriz BdP [" << k << " x " << V << "]...\n";
    vector<vector<int>> matriz(k, vector<int>(V, 0));

    for (int i = 0; i < k; i++)
        for (auto& [palabra, freq] : frecuencias[i])
            matriz[i][indice[palabra]] = freq;

    auto t_fin = chrono::high_resolution_clock::now();
    double t_serial = chrono::duration<double>(t_fin - t_inicio).count();

    // Paso 5: Escribir el CSV
    // El titulo se escapa porque puede tener coma. Las palabras no se escapan porque limpiarTexto ya quito todo lo que no sea letra
    cout << "Escribiendo " << ARCHIVO_SALIDA << "...\n";
    {
        ofstream csv(ARCHIVO_SALIDA);
        if (!csv.is_open()) {
            cerr << "[ERROR] No se pudo crear " << ARCHIVO_SALIDA << "\n";
            return 1;
        }

        csv << "titulo";
        for (const string& palabra : vocabulario)
            csv << "," << palabra;
        csv << "\n";

        for (int i = 0; i < k; i++) {
            csv << escaparCSV(titulos[i]);
            for (int j = 0; j < V; j++)
                csv << "," << matriz[i][j];
            csv << "\n";
        }
    }

    cout << "\n-----------------------------------------------------\n";
    cout << "  RESULTADOS -- Version Serial\n";
    cout << "-----------------------------------------------------\n";
    cout << "  Libros procesados : " << k << "\n";
    cout << "  Vocabulario global: " << V << " palabras\n";
    cout << "  Matriz generada   : " << ARCHIVO_SALIDA << "\n";
    cout << "  Tiempo serial     : " << t_serial << " s\n";
    cout << "-----------------------------------------------------\n";

    // Se guarda el tiempo a disco para que el programa MPI lo lea despues y calcule el speed-up
    ofstream tf("tiempo_serial.txt");
    tf << t_serial;

    return 0;
}
