// Punto de entrada provisional: solo demuestra que el esqueleto compila y enlaza
// con los hilos de la biblioteca estándar y con OpenCV. Se reemplaza en los PRs
// siguientes (carga de configuración, simulación, ventana).
#include <iostream>
#include <string>
#include <thread>

#include <opencv2/core/utility.hpp>

int main()
{
    std::string opencvVersion;

    // El hilo escribe y main lee después del join(): el join establece el
    // happens-before, así que no hay data race sobre opencvVersion.
    std::thread worker([&opencvVersion] { opencvVersion = cv::getVersionString(); });
    worker.join();

    std::cout << "delivery_sim: esqueleto OK (OpenCV " << opencvVersion << ")\n";
    return 0;
}
