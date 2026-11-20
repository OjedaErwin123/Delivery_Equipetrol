// Prueba de las versiones de CPU de H1 (EC2): el histograma de los tiempos de entrega. La de N hilos (histogramas
// privados por tramo) da exactamente lo mismo que la de 1 hilo con 1 a 7 hilos; la suma de las cajas es el número
// de entregas; los bordes de cada caja y la última (que junta todo lo de 255 minutos o más) caen donde deben; y el
// percentil de un minuto de resolución coincide con el que da ordenar todos los tiempos. Sale con 0 si todo está bien.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "histogram.hpp"
#include "log_line.hpp"

int main()
{
    bool ok = true;
    std::mt19937 random(5);
    bool parallelSame = true, totals = true, percentiles = true;
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    for (std::size_t n : {0, 1, 7, 1000, 100000}) {
        std::vector<std::uint32_t> ms(n);
        for (std::uint32_t& value : ms) {
            value = random() % (300u * kBinMs);  // hasta 300 minutos: también cae en la última caja
        }
        const Histogram reference = deliveryHistogram(ms);
        for (auto& pool : pools) {
            parallelSame &= deliveryHistogramParallel(ms, *pool) == reference;
        }
        totals &= std::accumulate(reference.begin(), reference.end(), std::uint64_t{0}) == n;
        if (n == 0) {
            percentiles &= percentileMinute(reference, 50) == -1;
            continue;
        }
        std::vector<std::uint32_t> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        for (double p : {50.0, 90.0, 99.0}) {
            // El primer tiempo con al menos p % de las entregas en o antes de él (rango k = ceil(p·n/100)).
            const auto k = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n) / 100.0));
            percentiles &= percentileMinute(reference, p) == static_cast<int>(binOf(sorted[std::max<std::size_t>(k, 1) - 1]));
        }
    }
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo, caja por caja", parallelSame);
    ok &= check("la suma de las cajas es el número de entregas", totals);
    ok &= check("p50, p90 y p99 (resolución de 1 minuto) = los del ordenamiento", percentiles);
    ok &= check("bordes: 59 999 ms en la caja 0, 60 000 en la 1, 4 h 15 min y 10 h en la última (255)",
                binOf(59999) == 0 && binOf(60000) == 1 && binOf(255u * kBinMs) == 255 && binOf(600u * kBinMs) == 255);
    return ok ? 0 : 1;
}
