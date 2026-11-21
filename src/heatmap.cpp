#include "heatmap.hpp"

#include <algorithm>
#include <cmath>
#include <future>

namespace {

// Las filas [y0, y1) de la salida. El orden de la suma (fila del filtro afuera, columna adentro) es el mismo que en
// la GPU.
void convolveRows(const Grid& in, const std::vector<float>& filter, Grid& out, int y0, int y1)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < in.width; ++x) {
            float sum = 0.0f;
            for (int fy = 0; fy < kTaps; ++fy) {
                const int sy = y + fy - kRadius;
                if (sy < 0 || sy >= in.height) {
                    continue;
                }
                for (int fx = 0; fx < kTaps; ++fx) {
                    const int sx = x + fx - kRadius;
                    if (sx >= 0 && sx < in.width) {
                        sum += filter[fy * kTaps + fx] * in.at(sx, sy);
                    }
                }
            }
            out.cells[static_cast<std::size_t>(y) * in.width + x] = sum;
        }
    }
}

}  // namespace

std::vector<float> gaussianFilter()
{
    std::vector<float> filter(kTaps * kTaps);
    double total = 0;
    for (int fy = 0; fy < kTaps; ++fy) {
        for (int fx = 0; fx < kTaps; ++fx) {
            const double dx = fx - kRadius, dy = fy - kRadius;
            const double w = std::exp(-(dx * dx + dy * dy) / (2.0 * kSigmaCells * kSigmaCells));
            filter[fy * kTaps + fx] = static_cast<float>(w);
            total += w;
        }
    }
    for (float& w : filter) {
        w = static_cast<float>(w / total);
    }
    return filter;
}

Grid convolve(const Grid& in, const std::vector<float>& filter)
{
    Grid out{in.width, in.height, std::vector<float>(in.cells.size())};
    convolveRows(in, filter, out, 0, in.height);
    return out;
}

Grid convolveParallel(const Grid& in, const std::vector<float>& filter, ThreadPool& pool)
{
    Grid out{in.width, in.height, std::vector<float>(in.cells.size())};
    const int parts = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(pool.size(), in.height)));
    std::vector<std::future<ThreadPool::Outcome<bool>>> done;
    for (int t = 0; t < parts; ++t) {
        const int y0 = in.height * t / parts, y1 = in.height * (t + 1) / parts;
        done.push_back(pool.submit([&in, &filter, &out, y0, y1] {  // se espera a todos antes de salir
            convolveRows(in, filter, out, y0, y1);
            return true;
        }));
    }
    for (auto& part : done) {
        part.get().take();
    }
    return out;
}

float maxDifference(const Grid& a, const Grid& b)
{
    float most = 0;
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        most = std::max(most, std::fabs(a.cells[i] - b.cells[i]));
    }
    return most;
}

float maxValue(const Grid& a)
{
    float most = 0;
    for (float v : a.cells) {
        most = std::max(most, std::fabs(v));
    }
    return most;
}
