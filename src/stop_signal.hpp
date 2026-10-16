#pragma once

#include <atomic>

// Detención por señal (SIGINT, SIGTERM).
//
// Un handler de señal interrumpe a un hilo en cualquier punto, incluso con un mutex tomado:
// si el handler tomara un lock o hiciera notify podría bloquearse contra ese mismo lock. Por
// eso lo único que hace es poner una bandera atómica lock-free, lo que el estándar permite en
// un handler. La lee el bucle de frames de la simulación, que ya despierta ~30 veces por
// segundo: no hace falta despertar a nadie y no hay espera activa.

// Instala el handler para SIGINT y SIGTERM. Llamarla una vez, antes de empezar la simulación.
void installStopSignalHandlers();

// La bandera que pone el handler. Una segunda señal la vuelve a poner: no cambia nada.
const std::atomic<bool>& stopSignalFlag();
