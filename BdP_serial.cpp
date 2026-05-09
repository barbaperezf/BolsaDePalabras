/**
 * ============================================================
 *  Bolsa de Palabras -- Version SERIAL
 * ============================================================
 *
 *  Descripcion:
 *    Lee una lista de URLs de Project Gutenberg, descarga cada
 *    libro dinamicamente, limpia y tokeniza el texto, construye
 *    un vocabulario global y genera una matriz BdP en CSV.
 *
 *  Compilacion:
 *    g++ -fdiagnostics-color=always -g BdP_serial.cpp -lcurl -o BdP_serial.exe
 *
 *  Ejecucion:
 *    ./BdP_serial.exe urls.txt
 *
 *  Formato de urls.txt (una URL por linea):
 *    https://www.gutenberg.org/cache/epub/1342/pg1342.txt
 *
 *  Salida:
 *    bdp_serial.csv  -- matriz Bolsa de Palabras
 * ============================================================
 */

#include <iostream>     //imprimir en pantalla
#include <fstream>      //leer y escribir archivos
#include <sstream>      //manejar strings como flujos (para tokenizar)
#include <string>       //manejar texto
#include <vector>       //estructuras dinamicas tipo array
#include <set>          //conjuntos ordenados (para vocabulario global)
#include <map>          //diccionarios clave-valor
#include <algorithm>    //funciones para manipular datos (sort, transform, etc)
#include <cctype>       //funciones para caracteres (isalpha, tolower, etc)
#include <chrono>       //medir tiempo de ejecucion
#include <curl/curl.h>  // librería externa para descargar URLs

using namespace std;

const string ARCHIVO_SALIDA = "bdp_serial.csv";

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

// ---------------------------------------------
//  MAIN
// ---------------------------------------------
int main(int argc, char* argv[]) {
 
    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <urls.txt>\n";
        return 1;
    }
 
    // -- 1. Leer lista de URLs -----------------
    vector<string> urls = leerURLs(argv[1]);
    int k = urls.size();
    if (k == 0) {
        cerr << "[ERROR] El archivo de URLs esta vacio.\n";
        return 1;
    }
 
    cout << "-----------------------------------------------------\n";
    cout << "  Bolsa de Palabras -- Version Serial\n";
    cout << "  Libros a procesar: " << k << "\n";
    cout << "-----------------------------------------------------\n\n";
 
    // -- Inicio del cronometro -----------------
    auto t_inicio = chrono::high_resolution_clock::now();
 
    // -- 2. Estructuras de datos principales --
    vector<string>           titulos(k);          // titulo real de cada libro
    vector<map<string, int>> frecuencias(k);      // frecuencia de palabras por libro
 
    // ── 3. Descargar, limpiar y contar ───────
    for (int i = 0; i < k; i++) {
        cout << "[" << (i+1) << "/" << k << "] " << urls[i] << "\n";
 
        string crudo = descargarLibro(urls[i]);
        if (crudo.empty()) {
            cerr << "  [WARN] No se pudo descargar, se omite.\n";
            titulos[i] = nombreDesdeURL(urls[i]);
            continue;
        }
 
        // Extraer titulo antes de recortar la cabecera
        titulos[i] = extraerTitulo(crudo, nombreDesdeURL(urls[i]));
 
        // Recortar cabecera/pie de Gutenberg y contar palabras
        string contenido    = recortarGutenberg(crudo);
        string limpio       = limpiarTexto(contenido);
        frecuencias[i]      = contarPalabras(limpio);
 
        cout << "  Titulo  : " << titulos[i] << "\n";
        cout << "  Palabras unicas: " << frecuencias[i].size() << "\n\n";
    }
 
    // -- 4. Construir vocabulario global ------
    //    Usamos set<string>: inserta y deduplica automaticamente
    cout << "Construyendo vocabulario global...\n";
 
    set<string> vocab_set;
    for (int i = 0; i < k; i++)
        for (auto& [palabra, _] : frecuencias[i])
            vocab_set.insert(palabra);
 
    // El set ya esta ordenado alfabeticamente (como CountVectorizer en sklearn)
    // Convertir a vector para acceso por indice
    vector<string> vocabulario(vocab_set.begin(), vocab_set.end());
    int V = vocabulario.size();
 
    // Mapa inverso: palabra → indice de columna en la matriz
    map<string, int> indice;
    for (int j = 0; j < V; j++)
        indice[vocabulario[j]] = j;
 
    cout << "Vocabulario global: " << V << " palabras unicas.\n\n";
 
    // -- 5. Construir matriz BdP ---------------
    cout << "Construyendo matriz BdP [" << k << " x " << V << "]...\n";
 
    vector<vector<int>> matriz(k, vector<int>(V, 0));
 
    for (int i = 0; i < k; i++)
        for (auto& [palabra, freq] : frecuencias[i])
            matriz[i][indice[palabra]] = freq;
 
    // -- Fin del cronometro --------------------
    auto t_fin    = chrono::high_resolution_clock::now();
    double t_serial = chrono::duration<double>(t_fin - t_inicio).count();
 
    // -- 6. Escribir CSV -----------------------
    cout << "Escribiendo " << ARCHIVO_SALIDA << "...\n";
    {
        ofstream csv(ARCHIVO_SALIDA);
        if (!csv.is_open()) {
            cerr << "[ERROR] No se pudo crear " << ARCHIVO_SALIDA << "\n";
            return 1;
        }
 
        // Encabezado: "titulo" + todas las palabras del vocabulario
        csv << "titulo";
        for (const string& palabra : vocabulario)
            csv << "," << palabra;
        csv << "\n";
 
        // Una fila por libro: titulo real + frecuencias
        for (int i = 0; i < k; i++) {
            csv << titulos[i];
            for (int j = 0; j < V; j++)
                csv << "," << matriz[i][j];
            csv << "\n";
        }
    }
 
    // -- 7. Resumen ----------------------------
    cout << "\n-----------------------------------------------------\n";
    cout << "  RESULTADOS -- Version Serial\n";
    cout << "-----------------------------------------------------\n";
    cout << "  Libros procesados : " << k << "\n";
    cout << "  Vocabulario global: " << V << " palabras\n";
    cout << "  Matriz generada   : " << ARCHIVO_SALIDA << "\n";
    cout << "  Tiempo serial     : " << t_serial << " s\n";
    cout << "-----------------------------------------------------\n";
 
    // Guardar tiempo para calcular speed-up con la version MPI
    ofstream tf("tiempo_serial.txt");
    tf << t_serial;
 
    return 0;
}