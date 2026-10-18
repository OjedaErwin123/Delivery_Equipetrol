#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>

#include "estimate.hpp"
#include "event_log.hpp"
#include "fleet.hpp"
#include "order_book.hpp"
#include "restaurant.hpp"
#include "shutdown.hpp"
#include "sim_clock.hpp"
#include "travel.hpp"

// Una moto. Su método run() corre en su propio hilo:
//   std::thread(&Courier::run, moto)   con `moto` un std::shared_ptr<Courier>
// El hilo guarda su copia del shared_ptr, así el objeto vive mientras el hilo lo use.
//
// La mochila es un monitor (como 07/example_42): los pedidos asignados, en orden de asignación,
// con el mutex de la moto y una variable de condición. El despachador los deja con assign(); el
// hilo de la moto los entrega uno a uno. Sin pedidos, duerme sin consumir CPU; termina cuando le
// cierran la mochila (close) y no le queda nada, o al abortar. Los viajes se pueden interrumpir:
// si se pide parar a mitad de un tramo, abandona lo que lleva (queda pendiente) y termina.
//
// Que la mochila no sea local del hilo es lo que permite, en el traspaso por avería, mover sus
// pedidos a otra moto de una vez. Por eso cada cambio de la mochila se registra en el log DENTRO
// de la misma sección crítica: nadie ve un pedido entregado que sigue en la mochila. Orden de
// locks: el de la moto, después Shutdown, después el log (nunca al revés).
//
// Con mapa (`world`), cada viaje recorre el camino mínimo por las calles a la velocidad de la
// flota: la moto publica su Trip (con su lock) y duerme hasta llegar. Sin pedidos queda quieta
// donde entregó. Sin mapa (pruebas), cada tramo dura `legMs`.
//
// Con restaurantes, en el restaurante hace fila (ticket) hasta tomar un puesto, espera la comida
// si no está lista y la entrega dura kHandoverMs, todo con el puesto tomado. Sin restaurantes
// (pruebas), la recogida es instantánea.
//
// La mochila lleva hasta bagCapacity pedidos asignados (el cupo lo controla la flota). Los entrega
// en el orden en que se los asignaron (FIFO). En cada visita a un restaurante recoge todos los
// pedidos de su mochila de ese restaurante (un solo ticket), esperando en el puesto a que esté
// lista la comida del último.
//
// Averías: al salir a cada viaje (con mapa) sortea si se avería, con su propio generador. Si toca,
// se avería en un instante al azar del viaje: queda quieta en ese punto de la calle, la flota deja
// de ofrecerla y se repara en kRepairMs; después sale de nuevo desde ahí (otro viaje, otro sorteo).
// Provisorio: conserva sus pedidos (el traspaso a otra moto llega en otro PR).
constexpr std::int64_t kRepairMs = 300000;  // 5 min simulados

class Courier {
public:
    // `clock`, `log` y `shutdown` los crea quien crea las motos y viven hasta después del join.
    // `world` puede ser nullptr (pruebas sin mapa): entonces cada tramo dura `legMs`.
    // `restaurants` puede ser nullptr (pruebas): entonces la recogida es instantánea.
    // `fleet` (con el índice de esta moto) lleva el cupo de la mochila: al terminar cada pedido la
    // moto libera el suyo. nullptr = sin límite (pruebas).
    Courier(std::string id, const SimClock& clock, EventLog& log, const Shutdown& shutdown, std::int64_t legMs,
            std::shared_ptr<const World> world = nullptr, std::shared_ptr<const Restaurants> restaurants = nullptr,
            std::shared_ptr<Fleet> fleet = nullptr, std::size_t index = 0, double breakdownProbability = 0,
            std::uint32_t seed = 0);
    Courier(const Courier&) = delete;
    Courier& operator=(const Courier&) = delete;

    const std::string& id() const { return _id; }

    // Asignarle un pedido (lo llama el despachador, ya con su hora de asignación): entra al final de
    // la mochila y despierta a la moto. closed si ya se cerró o abortó (el pedido no se mueve).
    OrderBook::Submit assign(Order&& order);
    // No habrá más asignaciones: la moto termina lo que tiene y sale.
    void close();
    // Detención: la moto deja lo que tenga (queda pendiente) y sale.
    void abort();

    void run();

    // El viaje actual (o la moto quieta), como copia: quien lo lee (la ventana, las estimaciones)
    // calcula la posición fuera del lock. Lo escribe solo el hilo de la moto, con _mutex.
    Trip trip() const;

    // Su plan (viaje + pedidos de la mochila en orden FIFO), como copia: lo usan las estimaciones.
    CourierPlan plan() const;

    // Estadísticas: solo las escribe el hilo de la moto (en run); leerlas después del join.
    int delivered() const { return _delivered; }
    std::int64_t totalDeliveryMs() const { return _totalDeliveryMs; }
    std::int64_t maxDeliveryMs() const { return _maxDeliveryMs; }
    int breakdowns() const { return _breakdowns; }

private:
    // Registra el evento salvo que se haya pedido parar o `t` ya pase el fin; false si no lo registró.
    bool logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant);

    // Un pedido en la mochila: lo asignado y si ya se recogió.
    struct Carried {
        Order order;
        bool picked = false;
    };
    // Lo que el hilo de la moto necesita saber del próximo pedido (una copia: se usa fuera del lock).
    struct Next {
        std::string id, restaurant, destination;
        bool picked;
    };
    // Duerme sin CPU hasta tener un pedido; std::nullopt si se abortó, o se cerró y no queda nada.
    std::optional<Next> waitForWork();
    // El pedido `id` de la mochila (lo busca por id: otros hilos pueden agregar pedidos). Con _mutex.
    std::deque<Carried>::iterator find(const std::string& id);

    enum class Leg { arrived, noPath, stopped };
    // Un tramo hasta `place` (id de restaurante o de nodo): por el camino mínimo si hay mapa, o
    // `legMs` si no. noPath: no hay camino desde donde está (solo con mapas de evaluadores).
    Leg travel(const std::string& place, bool isRestaurant);
    void publish(const Trip& trip);
    // ¿Se avería en este viaje? Si sí, en qué instante (entre la salida y la llegada).
    std::optional<std::int64_t> drawBreakdown(const Trip& trip);
    // La avería en `at`: quieta donde iba, fuera de la flota, y la reparación. false si se detuvo.
    bool breakDown(const Trip& trip, std::int64_t at);
    Leg pickUp(const std::string& restaurant);  // la visita a `restaurant`: recoge todo lo suyo
    bool deliver(const std::string& id);        // registra la entrega y lo saca de la mochila
    void drop(const std::string& id);           // no puede llevarlo (sin camino): queda pendiente
    void finished();  // terminó un pedido (entregado o imposible): libera su cupo en la flota

    const std::string _id;
    const SimClock& _clock;
    EventLog& _log;
    const Shutdown& _shutdown;
    const std::int64_t _legMs;  // sin mapa: duración simulada de cada tramo de viaje
    const std::shared_ptr<const World> _world;
    const std::shared_ptr<const Restaurants> _restaurants;
    const std::shared_ptr<Fleet> _fleet;
    const std::size_t _index;  // su lugar en la flota

    Place _at;  // dónde está (un nodo, o a mitad de una calle); solo lo usa el hilo de la moto

    // El monitor de la moto: _bag, _trip, _closed y _aborted solo se tocan con _mutex tomado.
    mutable std::mutex _mutex;
    std::condition_variable _work;  // hay un pedido nuevo, o se cerró / abortó
    std::deque<Carried> _bag;
    Trip _trip;
    bool _closed = false;
    bool _aborted = false;

    int _delivered = 0;
    std::int64_t _totalDeliveryMs = 0;  // suma de (entregado - creado)
    std::int64_t _maxDeliveryMs = 0;

    const double _breakdownProbability;
    std::mt19937 _rng;  // propio de esta moto: solo lo usa su hilo (como el del generador de pedidos)
    int _breakdowns = 0;
};
