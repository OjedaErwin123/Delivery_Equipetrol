#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "thread_pool.hpp"

// EC2, H1 (histograma): los tiempos de entrega (desde orderCreated hasta orderDelivered, en ms simulados) contados en
// cajas de 1 minuto. La caja b cuenta las entregas que tardaron [b, b+1) minutos; la última (kBins - 1) junta todas
// las de kBins - 1 minutos o más. Conteos enteros: la CPU de un hilo, la de N hilos y la GPU dan exactamente lo
// mismo, y la suma de las cajas es el número de entregas.
constexpr std::size_t kDeliveryBins = 256;
constexpr std::uint32_t kBinMs = 60000;

using Histogram = std::vector<std::uint64_t>;

// La caja de un tiempo de entrega.
inline std::size_t binOf(std::uint32_t ms)
{
    const std::size_t bin = ms / kBinMs;
    return bin < kDeliveryBins ? bin : kDeliveryBins - 1;
}

// CPU, 1 hilo: un recorrido que suma en un arreglo.
Histogram deliveryHistogram(const std::vector<std::uint32_t>& ms);

// CPU, N hilos: privatización (como los combiners de la carpeta 10). Cada trabajo del pool cuenta su tramo en SU
// histograma (nadie más lo toca: sin locks ni atómicos) y al final se suman una sola vez. Con un histograma
// compartido haría falta un mutex o un atómico por suma, y todos los hilos chocarían en las pocas cajas de los
// tiempos más comunes.
Histogram deliveryHistogramParallel(const std::vector<std::uint32_t>& ms, ThreadPool& pool);

// El percentil p (0 < p <= 100) con resolución de una caja: el primer minuto b tal que las cajas 0..b juntan al
// menos el p % de las entregas (por la suma acumulada). -1 si no hay entregas.
int percentileMinute(const Histogram& histogram, double p);
