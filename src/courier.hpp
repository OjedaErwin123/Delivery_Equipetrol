#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "event_log.hpp"
#include "order_book.hpp"
#include "shutdown.hpp"
#include "sim_clock.hpp"
#include "travel.hpp"

// Una moto. Su método run() corre en su propio hilo:
//   std::thread(&Courier::run, moto)   con `moto` un std::shared_ptr<Courier>
// El hilo guarda su copia del shared_ptr, así el objeto vive mientras el hilo lo use.
//
// Recibe pedidos en su bandeja (un OrderBook: FIFO, segura entre hilos y con cierre)
// y los entrega uno a uno. Sin pedidos, duerme en take() sin consumir CPU; termina
// cuando le cierran la bandeja y la vacía. Los viajes se pueden interrumpir: si se pide
// parar a mitad de un tramo, abandona el pedido (queda pendiente) y termina.
//
// Con mapa (`world`), cada viaje recorre el camino mínimo por las calles a la velocidad de la
// flota: la moto publica su Trip (con su lock) y duerme hasta llegar. Sin pedidos queda quieta
// en el nodo donde entregó. Sin mapa (pruebas), cada tramo dura `legMs`.
class Courier {
public:
    // `clock`, `log` y `shutdown` los crea quien crea las motos y viven hasta después del join.
    // `world` puede ser nullptr (pruebas sin mapa): entonces cada tramo dura `legMs`.
    Courier(std::string id, const SimClock& clock, EventLog& log, const Shutdown& shutdown, std::int64_t legMs,
            std::shared_ptr<const World> world = nullptr);
    Courier(const Courier&) = delete;
    Courier& operator=(const Courier&) = delete;

    const std::string& id() const { return _id; }
    OrderBook& inbox() { return _inbox; }

    void run();

    // El viaje actual (o la moto quieta), como copia: quien lo lee (la ventana, las estimaciones)
    // calcula la posición fuera del lock. Lo escribe solo el hilo de la moto, con _mutex.
    Trip trip() const;

    // Estadísticas: solo las escribe el hilo de la moto (en run); leerlas después del join.
    int delivered() const { return _delivered; }
    std::int64_t totalDeliveryMs() const { return _totalDeliveryMs; }
    std::int64_t maxDeliveryMs() const { return _maxDeliveryMs; }

private:
    // Registra el evento salvo que se haya pedido parar o `t` ya pase el fin; false si no lo registró.
    bool logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant);

    enum class Leg { arrived, noPath, stopped };
    // Un tramo hasta `place` (id de restaurante o de nodo): por el camino mínimo si hay mapa, o
    // `legMs` si no. noPath: no hay camino desde donde está (solo con mapas de evaluadores).
    Leg travel(const std::string& place, bool isRestaurant);
    void publish(const Trip& trip);

    const std::string _id;
    const SimClock& _clock;
    EventLog& _log;
    const Shutdown& _shutdown;
    const std::int64_t _legMs;  // sin mapa: duración simulada de cada tramo de viaje
    const std::shared_ptr<const World> _world;
    OrderBook _inbox;

    std::size_t _node;              // dónde está; solo lo usa el hilo de la moto
    mutable std::mutex _mutex;      // protege _trip, que leen otros hilos
    Trip _trip;

    int _delivered = 0;
    std::int64_t _totalDeliveryMs = 0;  // suma de (entregado - creado)
    std::int64_t _maxDeliveryMs = 0;
};
