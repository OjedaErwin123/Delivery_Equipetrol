#include "gpu/gpu_owner.hpp"

#include <string>
#include <utility>

GpuOwner::GpuOwner(std::size_t capacity)
{
    // Se espera el resultado: así un error (sin GPU, sin memoria) sale de este constructor, en el hilo que lo creó.
    _deviceName = _thread
                      .submit([this, capacity] {
                          _nearest = std::make_unique<NearestGpu>(capacity);
                          return NearestGpu::deviceName();
                      })
                      .get()
                      .take();
}

GpuOwner::~GpuOwner()
{
    // La memoria de la GPU se libera en el hilo dueño. Los trabajos anteriores ya salieron de la cola (es FIFO y
    // de un solo hilo): este corre después de todos.
    _thread
        .submit([this] {
            _nearest.reset();
            return true;
        })
        .get();
}

std::future<ThreadPool::Outcome<GpuOwner::Nearest>> GpuOwner::nearest(std::vector<Coordinates> at, Coordinates shop,
                                                                      std::size_t limit)
{
    return _thread.submit([this, at = std::move(at), shop, limit] {
        Nearest result;
        result.superset = _nearest->superset(at, shop, limit, &result.timing);
        return result;
    });
}
