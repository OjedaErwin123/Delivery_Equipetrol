#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Un thread pool hecho a mano (como 09/example_55): una cola de trabajos que es un monitor (07)
// y un número FIJO de hilos que la vacían. Sirve para las estimaciones del despachador: con 64
// motos, cada pedido genera 64 estimaciones, pero no 64 hilos: entran a la cola y los hilos del
// pool las van sacando. Lo que crece con las motos son las tareas, no los hilos.
//
// Cada trabajo devuelve su resultado por un std::promise propio: un canal de un solo uso entre el
// hilo del pool que lo calcula y el hilo que espera el std::future (04). Si el trabajo lanza, la
// excepción viaja por el mismo canal y sale en future.get(). A diferencia del future de
// std::async, este no bloquea al destruirse: quien deja de esperar una estimación tardía no se
// queda colgado; el trabajo termina en el pool y su resultado se descarta.
//
// _jobs y _closed solo se tocan con _mutex tomado; cada trabajo corre FUERA del lock.
class ThreadPool {
public:
    // hardware_concurrency() es una pista y puede devolver 0 (no sabe): entonces 2, lo mínimo para
    // que las estimaciones corran en paralelo.
    static std::size_t defaultSize();

    explicit ThreadPool(std::size_t threads);
    // Cierra la cola, DESCARTA los trabajos que todavía no empezaron (sus futures reciben
    // std::future_error broken_promise) y hace join de todos los hilos: ninguno queda abandonado.
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t size() const { return _threads.size(); }

    // Encola `task` (sin argumentos, devuelve un valor) y devuelve el future de su resultado.
    template <typename Task>
    std::future<std::invoke_result_t<Task>> submit(Task task)
    {
        using Result = std::invoke_result_t<Task>;
        static_assert(!std::is_void_v<Result>, "el trabajo debe devolver un resultado");
        // std::function exige algo copiable y el promise es move-only: se comparte con shared_ptr.
        auto promise = std::make_shared<std::promise<Result>>();
        std::future<Result> future = promise->get_future();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _jobs.emplace_back([promise, task = std::move(task)]() mutable {
                try {
                    promise->set_value(task());
                } catch (...) {
                    promise->set_exception(std::current_exception());  // el error cruza de hilo
                }
            });
        }
        _ready.notify_one();
        return future;
    }

private:
    void work();

    std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<std::function<void()>> _jobs;
    bool _closed = false;
    std::vector<std::thread> _threads;  // último: se crean cuando la cola ya existe
};
