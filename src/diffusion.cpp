#include "diffusion.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace {

// Un paso de las filas [y0, y1) de `from` a `to` (solo interiores: los bordes valen 0 en los dos buffers). El orden
// de las operaciones es el mismo que en la GPU.
void stepRows(int w, int h, const std::vector<float>& from, std::vector<float>& to, int y0, int y1)
{
    for (int y = std::max(y0, 1); y < std::min(y1, h - 1); ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const std::size_t c = static_cast<std::size_t>(y) * w + x;
            const float around = from[c - w] + from[c + w] + from[c + 1] + from[c - 1];
            to[c] = kKeep * (from[c] + kSpread * (around - 4.0f * from[c]));
        }
    }
}

Grid withZeroBorder(const Grid& start)
{
    Grid g = start;
    for (int x = 0; x < g.width; ++x) {
        g.cells[x] = 0.0f;
        g.cells[static_cast<std::size_t>(g.height - 1) * g.width + x] = 0.0f;
    }
    for (int y = 0; y < g.height; ++y) {
        g.cells[static_cast<std::size_t>(y) * g.width] = 0.0f;
        g.cells[static_cast<std::size_t>(y) * g.width + g.width - 1] = 0.0f;
    }
    return g;
}

}  // namespace

Grid diffuse(const Grid& start, int steps)
{
    Grid a = withZeroBorder(start), b = a;
    for (int s = 0; s < steps; ++s) {
        stepRows(a.width, a.height, a.cells, b.cells, 0, a.height);
        std::swap(a, b);
    }
    return a;
}

Grid diffuseParallel(const Grid& start, int steps, ThreadPool& pool)
{
    Grid a = withZeroBorder(start), b = a;
    const int parts = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(pool.size(), a.height)));
    for (int s = 0; s < steps; ++s) {
        std::vector<std::future<ThreadPool::Outcome<bool>>> done;
        for (int t = 0; t < parts; ++t) {
            const int y0 = a.height * t / parts, y1 = a.height * (t + 1) / parts;
            done.push_back(pool.submit([&a, &b, y0, y1] {
                stepRows(a.width, a.height, a.cells, b.cells, y0, y1);
                return true;
            }));
        }
        for (auto& part : done) {
            part.get().take();
        }
        std::swap(a, b);
    }
    return a;
}
