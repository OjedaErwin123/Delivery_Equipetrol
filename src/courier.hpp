#pragma once

#include <cstdint>
#include <string>

#include "event_log.hpp"
#include "order_book.hpp"
#include "sim_clock.hpp"

// Una moto. Su método run() corre en su propio hilo:
//   std::thread(&Courier::run, moto)   con `moto` un std::shared_ptr<Courier>
// El hilo guarda su copia del shared_ptr, así el objeto vive mientras el hilo lo use.
//
// Recibe pedidos en su bandeja (un OrderBook: FIFO, segura entre hilos y con cierre)
// y los entrega uno a uno. Sin pedidos, duerme en take() sin consumir CPU; termina
// cuando le cierran la bandeja y la vacía.
class Courier {
public:
    // `clock` y `log` los crea quien crea las motos y viven hasta después del join.
    Courier(std::string id, const SimClock& clock, EventLog& log, std::int64_t legMs);
    Courier(const Courier&) = delete;
    Courier& operator=(const Courier&) = delete;

    const std::string& id() const { return _id; }
    OrderBook& inbox() { return _inbox; }

    void run();

    // Estadísticas: solo las escribe el hilo de la moto (en run); leerlas después del join.
    int delivered() const { return _delivered; }
    std::int64_t totalDeliveryMs() const { return _totalDeliveryMs; }
    std::int64_t maxDeliveryMs() const { return _maxDeliveryMs; }

private:
    void logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant);

    const std::string _id;
    const SimClock& _clock;
    EventLog& _log;
    const std::int64_t _legMs;  // duración simulada de cada tramo de viaje (v0: fija)
    OrderBook _inbox;

    int _delivered = 0;
    std::int64_t _totalDeliveryMs = 0;  // suma de (entregado - creado)
    std::int64_t _maxDeliveryMs = 0;
};
