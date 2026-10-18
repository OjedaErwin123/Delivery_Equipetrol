// Prueba del thread pool: resultados correctos, los trabajos corren en paralelo pero nunca más
// a la vez que hilos, una excepción llega por el future y el pool sigue andando, esperar con
// límite de tiempo y soltar un future tardío no bloquea, y al destruirse descarta lo que no
// empezó y hace join de todo. Sale con 0 si todo está bien.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "log_line.hpp"
#include "thread_pool.hpp"

namespace {

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

}  // namespace

int main()
{
    bool ok = true;
    std::cout << "hardware_concurrency() = " << std::thread::hardware_concurrency()
              << " -> tamaño por defecto del pool = " << ThreadPool::defaultSize() << "\n";
    ok &= check("tamaño por defecto: los núcleos que informe el sistema, o 2 si no informa",
                ThreadPool::defaultSize() == (std::thread::hardware_concurrency() == 0
                                                  ? 2u
                                                  : std::thread::hardware_concurrency()));

    // 1. Resultados: 200 trabajos, cada uno con su future.
    {
        ThreadPool pool(4);
        std::vector<std::future<int>> results;
        for (int i = 0; i < 200; ++i) {
            results.push_back(pool.submit([i] { return i * i; }));
        }
        bool right = true;
        for (int i = 0; i < 200; ++i) {
            right = right && results[i].get() == i * i;
        }
        ok &= check("200 trabajos: cada future trae su resultado", right);
    }

    // 2. Paralelo, pero nunca más trabajos a la vez que hilos: 12 trabajos de ~30 ms con 4 hilos.
    {
        ThreadPool pool(4);
        std::atomic<int> running(0);
        std::atomic<int> most(0);
        std::vector<std::future<int>> results;
        const auto start = Clock::now();
        for (int i = 0; i < 12; ++i) {
            results.push_back(pool.submit([&running, &most] {
                const int now = ++running;
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));  // representa trabajo
                --running;
                return 1;
            }));
        }
        for (auto& r : results) {
            r.get();
        }
        const long long took = msSince(start);
        std::cout << "12 trabajos de 30 ms con 4 hilos: " << took << " ms, máximo a la vez " << most << "\n";
        ok &= check("corren en paralelo (más de 1 a la vez) y nunca más que los hilos (4)",
                    most > 1 && most <= 4 && took < 12 * 30);
    }

    // 3. Una excepción en un trabajo llega por future.get() y el pool sigue andando.
    {
        ThreadPool pool(2);
        std::future<int> bad = pool.submit([]() -> int { throw std::runtime_error("sin camino al restaurante"); });
        std::string message;
        try {
            bad.get();
        } catch (const std::runtime_error& e) {
            message = e.what();
        }
        ok &= check("la excepción cruza de hilo y sale en get()", message == "sin camino al restaurante");
        ok &= check("después del error, el pool sigue andando", pool.submit([] { return 7; }).get() == 7);
    }

    // 4. Esperar con límite: un trabajo lento no llega a tiempo; soltar su future no bloquea.
    {
        ThreadPool pool(2);
        const auto start = Clock::now();
        {
            std::future<int> late = pool.submit([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return 1;
            });
            const bool timedOut =
                late.wait_until(Clock::now() + std::chrono::milliseconds(20)) == std::future_status::timeout;
            ok &= check("wait_until: el trabajo lento no llega en 20 ms (timeout)", timedOut);
        }  // aquí se destruye el future tardío: con std::async esto esperaría los 200 ms
        const long long released = msSince(start);
        std::cout << "soltar el future tardío tomó " << released << " ms\n";
        ok &= check("soltar el future tardío no espera al trabajo (a diferencia de std::async)", released < 150);
    }  // el destructor del pool espera a que termine el trabajo en curso (join)

    // 5. Al destruirse, lo que no empezó se descarta: su future recibe broken_promise.
    std::future<int> neverRan;
    {
        ThreadPool pool(1);
        std::future<int> slow = pool.submit([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // el único hilo ya tomó el lento
        neverRan = pool.submit([] { return 2; });
    }  // join: espera el lento; el segundo nunca empezó
    bool broken = false;
    try {
        neverRan.get();
    } catch (const std::future_error& e) {
        broken = e.code() == std::future_errc::broken_promise;
    }
    ok &= check("al destruirse: lo que no empezó se descarta (broken_promise) y todos los hilos se unen", broken);
    return ok ? 0 : 1;
}
