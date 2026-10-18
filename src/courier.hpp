#pragma once

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
// Recibe pedidos en su bandeja (un OrderBook: FIFO, segura entre hilos y con cierre)
// y los entrega uno a uno. Sin pedidos, duerme en take() sin consumir CPU; termina
// cuando le cierran la bandeja y la vacía. Los viajes se pueden interrumpir: si se pide
// parar a mitad de un tramo, abandona el pedido (queda pendiente) y termina.
//
// Con mapa (`world`), cada viaje recorre el camino mínimo por las calles a la velocidad de la
// flota: la moto publica su Trip (con su lock) y duerme hasta llegar. Sin pedidos queda quieta
// en el nodo donde entregó. Sin mapa (pruebas), cada tramo dura `legMs`.
//
// Con restaurantes, en el restaurante hace fila (ticket) hasta tomar un puesto, espera la comida
// si no está lista y la entrega dura kHandoverMs, todo con el puesto tomado. Sin restaurantes
// (pruebas), la recogida es instantánea.
//
// La mochila: la moto lleva hasta bagCapacity pedidos asignados (el cupo lo controla la flota).
// Los entrega en el orden en que se los asignaron (FIFO). En cada visita a un restaurante recoge
// todos los pedidos de su mochila de ese restaurante (un solo ticket), esperando en el puesto a
// que esté lista la comida del último.
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

    // Asignarle un pedido (lo llama el despachador, ya con su hora de asignación): lo anota en el
    // plan y lo deja en la bandeja, así el plan siempre tiene TODOS los asignados aunque la moto
    // todavía no los haya pasado a su mochila.
    OrderBook::Submit assign(Order&& order);
    OrderBook& inbox() { return _inbox; }

    void run();

    // El viaje actual (o la moto quieta), como copia: quien lo lee (la ventana, las estimaciones)
    // calcula la posición fuera del lock. Lo escribe solo el hilo de la moto, con _mutex.
    Trip trip() const;

    // Su plan (viaje + pedidos asignados en orden FIFO), como copia: lo usan las estimaciones.
    CourierPlan plan() const;

    // Estadísticas: solo las escribe el hilo de la moto (en run); leerlas después del join.
    int delivered() const { return _delivered; }
    std::int64_t totalDeliveryMs() const { return _totalDeliveryMs; }
    std::int64_t maxDeliveryMs() const { return _maxDeliveryMs; }
    int breakdowns() const { return _breakdowns; }

private:
    // Registra el evento salvo que se haya pedido parar o `t` ya pase el fin; false si no lo registró.
    bool logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant);

    // Un pedido en la mochila: lo asignado y si ya se recogió. La mochila (un deque en run) es
    // local del hilo de la moto: nadie más la toca, así que no necesita lock.
    struct Carried {
        Order order;
        bool picked = false;
    };

    enum class Leg { arrived, noPath, stopped };
    // Un tramo hasta `place` (id de restaurante o de nodo): por el camino mínimo si hay mapa, o
    // `legMs` si no. noPath: no hay camino desde donde está (solo con mapas de evaluadores).
    Leg travel(const std::string& place, bool isRestaurant);
    void publish(const Trip& trip);
    // ¿Se avería en este viaje? Si sí, en qué instante (entre la salida y la llegada).
    std::optional<std::int64_t> drawBreakdown(const Trip& trip);
    // La avería en `at`: quieta donde iba, fuera de la flota, y la reparación. false si se detuvo.
    bool breakDown(const Trip& trip, std::int64_t at);
    void collect(std::deque<Carried>& bag);  // pasa a la mochila lo que haya en la bandeja, sin esperar
    Leg pickUp(std::deque<Carried>& bag);     // la visita al restaurante del primer pedido de la mochila
    void finished();  // terminó un pedido (entregado o imposible): libera su cupo en la flota
    // El plan acompaña a la mochila: la mochila es siempre el principio de plan.orders (mismo orden).
    void markPicked(std::size_t i);  // el i-ésimo de la mochila ya se recogió
    void dropFirst();                // el primero de la mochila terminó (entregado o imposible)

    const std::string _id;
    const SimClock& _clock;
    EventLog& _log;
    const Shutdown& _shutdown;
    const std::int64_t _legMs;  // sin mapa: duración simulada de cada tramo de viaje
    const std::shared_ptr<const World> _world;
    const std::shared_ptr<const Restaurants> _restaurants;
    const std::shared_ptr<Fleet> _fleet;
    const std::size_t _index;  // su lugar en la flota
    OrderBook _inbox;

    Place _at;                      // dónde está (un nodo, o a mitad de una calle); solo lo usa el hilo de la moto
    mutable std::mutex _mutex;      // protege _plan, que leen otros hilos
    CourierPlan _plan;

    int _delivered = 0;
    std::int64_t _totalDeliveryMs = 0;  // suma de (entregado - creado)
    std::int64_t _maxDeliveryMs = 0;

    const double _breakdownProbability;
    std::mt19937 _rng;  // propio de esta moto: solo lo usa su hilo (como el del generador de pedidos)
    int _breakdowns = 0;
};
