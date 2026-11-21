#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "road_graph.hpp"

// EC2, R1 en la GPU: el prefiltro de las motos más cercanas. Para cada moto calcula la distancia haversine al
// restaurante (un hilo por moto) y con reducciones por bloque encuentra la (limit+1)-ésima menor clave
// (distancia, posición). Devuelve todas las posiciones cuya distancia no supera esa clave + 2·kNearestToleranceM:
// un subconjunto que seguro contiene a las `limit` mejores aunque la GPU redondee distinto que la CPU, y que
// tiene más de `limit` elementos. La decisión la toma la CPU con nearestWithin (nearest.hpp): el resultado es el
// mismo que nearestOf sobre todas las motos.
//
// Por qué alcanza el margen: si la GPU y la CPU difieren en menos de d por moto, las limit+1 motos con menor
// clave en la GPU tienen en la CPU distancia <= g + d (g = la limit+1-ésima de la GPU), así que la limit-ésima de
// la CPU es <= g + d; cualquier moto entre las `limit` mejores de la CPU tiene en la GPU <= g + 2d.
//
// Sin tipos de CUDA en esta interfaz: la incluyen archivos de C++. La implementación (nearest_gpu.cu) se compila
// con nvcc (USE_CUDA=ON) o como C++ con la emulación de cuda_compat.h (solo en las pruebas).
//
// No es seguro entre hilos: lo usa un solo hilo, el dueño de la GPU (GpuOwner), o una prueba.
class NearestGpu {
public:
    // Reserva en la GPU lugar para `capacity` motos (una sola vez: ninguna llamada reserva memoria).
    explicit NearestGpu(std::size_t capacity);
    ~NearestGpu();
    NearestGpu(const NearestGpu&) = delete;
    NearestGpu& operator=(const NearestGpu&) = delete;

    // Tiempos de una llamada, en ms. kernelMs: solo los kernels (eventos de CUDA, con los datos ya en la GPU);
    // endToEndMs: copia de ida + kernels + copia de vuelta, medido en el host después de sincronizar.
    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    // Las posiciones (índices de `at`) del subconjunto. Con at.size() <= limit, todas. at.size() <= capacity.
    std::vector<std::size_t> superset(const std::vector<Coordinates>& at, Coordinates shop, std::size_t limit,
                                      Timing* timing = nullptr);

    // Cuántos bloques de 256 hilos se lanzan para n motos (para el informe: grid y ocupación).
    static unsigned blocksFor(std::size_t n);
    // El nombre del dispositivo (p. ej. "Tesla T4") o el de la emulación.
    static std::string deviceName();

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity;
};
