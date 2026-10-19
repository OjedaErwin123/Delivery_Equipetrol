// Prueba de las supresiones de TSan (DP.11): ¿OpenCV highgui da reportes de TSan/Valgrind en WSLg?
// ¿Qué pasa sin pantalla? Abre una ventana, dibuja ~2 s a 30 fps con otro hilo moviendo un punto.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#if defined(SUPPRESS_QT)
// TSan lee esta función sola al arrancar: suprime reportes cuya pila pasa por las bibliotecas de Qt.
extern "C" const char* __tsan_default_suppressions()
{
    return "race:libQt5Core.so\n"
           "race:libQt5Gui.so\n"
           "race:libQt5Widgets.so\n"
           "race:libQt5DBus.so\n"
           "race:libQt5XcbQpa.so\n"
           "race:libdbus-1.so\n"
           "deadlock:libdbus-1.so\n";
}
#endif

int racy = 0;  // carrera NUESTRA a propósito (con "race" como 2do argumento): TSan la tiene que ver igual

int main(int argc, char** argv)
{
    const bool injectRace = argc > 2 && std::string(argv[2]) == "race";
    cv::setNumThreads(0);  // OpenCV sin paralelismo interno (su backend es TBB): dibuja en este hilo
    std::cout << "hilos de OpenCV: " << cv::getNumThreads() << "\n";
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    std::cout << "DISPLAY=" << (display ? display : "(nada)") << " WAYLAND_DISPLAY=" << (wayland ? wayland : "(nada)")
              << "\n";
    cv::Mat map = argc > 1 ? cv::imread(argv[1]) : cv::Mat(600, 800, CV_8UC3, cv::Scalar(230, 230, 230));
    std::cout << "imagen: " << map.cols << "x" << map.rows << "\n";
    try {
        cv::namedWindow("spike", cv::WINDOW_AUTOSIZE);
    } catch (const cv::Exception& e) {
        std::cout << "namedWindow lanzó: " << e.what() << "\n";
        return 3;
    }
    std::mutex m;
    double x = 0;
    std::atomic<bool> done(false);
    std::thread mover([&] {
        while (!done) {
            {
                std::lock_guard<std::mutex> lock(m);
                x += 4;
            }
            if (injectRace) {
                ++racy;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int frames = 0;
    bool closed = false;
    while (std::chrono::steady_clock::now() < end) {
        cv::Mat frame = map.clone();
        double px;
        {
            std::lock_guard<std::mutex> lock(m);
            px = x;
        }
        cv::circle(frame, {static_cast<int>(px) % frame.cols, frame.rows / 2}, 6, cv::Scalar(255, 0, 0), cv::FILLED);
        cv::putText(frame, "(c) OpenStreetMap contributors", {10, frame.rows - 10}, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
        if (injectRace) {
            ++racy;
        }
        cv::imshow("spike", frame);
        cv::waitKey(1);
        ++frames;
        if (cv::getWindowProperty("spike", cv::WND_PROP_VISIBLE) < 1) {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    done = true;
    mover.join();
    cv::destroyAllWindows();
    std::cout << "frames: " << frames << (closed ? " (cerrada por el usuario)" : "") << "\n";
    return 0;
}
