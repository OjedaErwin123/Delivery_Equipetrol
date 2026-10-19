#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <ostream>
#include <string>

// Un evento del registro (sección 6.2 del enunciado). Los campos de texto vacíos
// no se escriben: cada evento lleva solo los que le corresponden.
struct Event {
    std::int64_t t = 0;       // milisegundos simulados desde el inicio
    std::string event;        // orderCreated, orderAssigned, ...
    std::string order;
    std::string courier;
    std::string restaurant;
    std::string from;         // orderReassigned
    std::string to;           // orderReassigned
    std::string reason;       // orderRejected: queueFull | unreachable | noCourier
    std::string destination;  // orderCreated: NO va en la línea; solo para los pedidos activos de la foto
};

// Los contadores del resumen (sección 6.3) en vivo, y los pedidos activos con su punto de entrega.
// Se actualizan en write(), con el mismo lock con que se escribe la línea: una foto (stats()) es
// siempre exactamente lo que ya está en el log, así el panel de la ventana nunca lo contradice.
struct LiveStats {
    int created = 0, delivered = 0, rejected = 0, reassigned = 0, breakdowns = 0;
    std::int64_t totalDeliveryMs = 0, maxDeliveryMs = 0;  // de la creación a la entrega
    std::map<std::string, int> deliveredPerCourier;
    std::map<std::string, std::string> activeDestinations;  // pedido sin terminar -> nodo de entrega

    int pending() const { return created - delivered - rejected; }
    std::int64_t avgDeliveryMs() const { return delivered > 0 ? totalDeliveryMs / delivered : 0; }
};

// Arma la línea JSON de un evento (sin el salto de línea). No toca estado
// compartido, así que cualquier hilo puede llamarla sin lock.
std::string toJsonLine(const Event& e);

// Registro de eventos compartido por todos los hilos: un monitor sobre el stream.
//
// El stream solo se escribe dentro de write(), con _mutex tomado, y cada llamada
// escribe una línea completa: dos eventos nunca se mezclan en una línea y no hay
// data race sobre el stream. La línea se arma ANTES de tomar el lock, así la
// sección crítica es solo la escritura.
class EventLog {
public:
    explicit EventLog(std::ostream& out) : _out(out) {}
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    void write(const Event& e);

    // Una copia de los contadores, con el lock: lo que el log tiene escrito hasta ahora.
    LiveStats stats() const;

private:
    void count(const Event& e);  // con _mutex tomado

    mutable std::mutex _mutex;
    std::ostream& _out;  // no es dueño: el stream lo crea y lo mantiene vivo quien crea el registro
    LiveStats _stats;
    std::map<std::string, std::int64_t> _createdAt;  // pedido sin terminar -> t de orderCreated
};
