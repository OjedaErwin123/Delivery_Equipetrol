#include "stop_signal.hpp"

#include <csignal>

namespace {

// Lock-free es lo que hace segura a la bandera dentro del handler: si no lo fuera, la
// implementación usaría un lock interno y volvería el problema. Se comprueba al compilar.
static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "std::atomic<bool> debe ser lock-free para usarse en un handler de señal");

std::atomic<bool> stopRequested(false);

}  // namespace

extern "C" void onStopSignal(int signal)
{
    stopRequested.store(true);
    // Algunas plataformas vuelven al comportamiento por defecto tras la primera señal (que
    // terminaría el proceso de golpe ante una segunda): se reinstala para que también se ignore.
    std::signal(signal, onStopSignal);
}

void installStopSignalHandlers()
{
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
}

const std::atomic<bool>& stopSignalFlag()
{
    return stopRequested;
}
