Bolsa de Palabras con MPI
Cómputo Paralelo y en la Nube - Proyecto de Clausura
Fernando Barba y Nicolas Robles

Implementacion serial y paralela con MPI que recibe una lista de URLs de
Project Gutenberg, descarga los libros dinamicamente y genera una matriz
Bolsa de Palabras en formato CSV.


Compilacion

    g++ -std=c++17 -O2 BdP_serial.cpp -lcurl -o BdP_serial
    mpicxx -std=c++17 -O2 BdP_MPI.cpp -lcurl -o BdP_MPI


Ejecucion

Primero hay que correr el serial y despues el paralelo. El orden importa
porque al terminar la version serial guarda su tiempo en tiempo_serial.txt,
y la version paralela lo lee de ahi para calcular el speed-up. Como son dos
ejecutables separados que corren en momentos distintos, no comparten memoria
y la unica forma de pasarse el dato es por disco. Si se corre primero el
MPI igual funciona y genera el CSV, pero no imprime speed-up.

    ./BdP_serial urls.txt
    mpirun -np 4 ./BdP_MPI urls.txt

El programa funciona para cualquier numero k de libros y cualquier numero q
de procesos, no estan hardcodeados.
