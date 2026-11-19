#pragma once

#include <cstddef>
#include <future>
#include <memory>
#include <vector>

#include "gpu/nearest_gpu.hpp"
#include "thread_pool.hpp"

// EC2: el hilo dueño de la GPU. Toda llamada a CUDA (crear el contexto, reservar memoria, copiar, lanzar
// kernels, liberar) pasa por este único hilo: un ThreadPool de 1 hilo (la cola monitor de 07 + un promise por
// trabajo de 04, como el pool de estimaciones). Los demás hilos le mandan trabajos y esperan el future con el
// plazo que quieran (el despachador, quoteTimeoutMs): mientras corre un kernel, las motos siguen viajando y el
// bucle de frames sigue dibujando. Un error de CUDA llega por el future como excepción, no tira el programa.
//
// Por qué un solo hilo: el contexto de CUDA y los buffers reservados son estado de este objeto; si los tocaran
// varios hilos habría que protegerlos con un lock y se serializarían igual en la GPU. Así, `_nearest` lo crea,
// lo usa y lo destruye solo el hilo de `_thread` (ningún otro lo toca): no hay data race posible sobre él.
class GpuOwner {
public:
    // Crea el hilo y, en él, prepara la GPU para hasta `capacity` motos. Lanza si no hay GPU o falla la reserva.
    explicit GpuOwner(std::size_t capacity);
    // Libera la GPU en su hilo y después hace join del hilo (ThreadPool).
    ~GpuOwner();
    GpuOwner(const GpuOwner&) = delete;
    GpuOwner& operator=(const GpuOwner&) = delete;

    struct Nearest {
        std::vector<std::size_t> superset;  // posiciones de `at` (ver NearestGpu::superset)
        NearestGpu::Timing timing;
    };
    // Encola el prefiltro de las más cercanas. `at` viaja por valor: el trabajo puede terminar después de que
    // quien lo pidió dejó de esperar.
    std::future<ThreadPool::Outcome<Nearest>> nearest(std::vector<Coordinates> at, Coordinates shop, std::size_t limit);

    // El nombre del dispositivo (leído en el hilo dueño al crear el objeto).
    const std::string& deviceName() const { return _deviceName; }

private:
    std::unique_ptr<NearestGpu> _nearest;  // solo lo toca el hilo de _thread
    std::string _deviceName;
    ThreadPool _thread{1};                  // se destruye primero: join antes de que _nearest deje de existir
};
