#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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
// de ofrecerla, traspasa toda su mochila (con `handOver`: elige el receptor y espera si hace falta)
// y queda quieta ahí. La comida que ya había recogido sigue con ella: el receptor la viene a buscar
// (kRescueMs en el punto), y recién cuando le retiraron toda empieza la reparación (kRepairMs). Si
// no queda ninguna otra moto que pueda trabajar (todas averiadas o terminadas), se repara igual y
// sigue con sus pedidos.
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
    // Lo mismo, revalidando con el lock de la moto (check-then-act): el despachador reservó el cupo
    // antes, y entre medio la moto pudo averiarse. Si está averiada devuelve `full` (ahora no puede) y
    // el pedido no se mueve. Si no, `logAssigned` registra orderAssigned DENTRO de la misma sección
    // crítica en que el pedido entra a la mochila (si devuelve false, se detuvo: `closed`, sin moverlo).
    OrderBook::Submit assign(Order&& order, const std::function<bool()>& logAssigned);
    // No habrá más asignaciones: la moto termina lo que tiene y sale.
    void close();
    // Detención: la moto deja lo que tenga (queda pendiente) y sale.
    void abort();

    void run();

    // Quién recibe la mochila cuando esta moto se avería: lo fija quien crea las motos, ANTES de
    // lanzar los hilos. Devuelve false si se detuvo. Sin esto (pruebas), la moto conserva sus pedidos.
    void onBreakdown(std::function<bool(Courier&)> handOver) { _handOver = std::move(handOver); }
    std::size_t index() const { return _index; }

    // El traspaso de TODA la mochila de esta moto a `to`, de una vez (todo o nada: ningún observador
    // ve un pedido en las dos mochilas ni en ninguna). Toma los dos mutex a la vez con
    // std::scoped_lock, así dos motos que se traspasan mutuamente al mismo tiempo no se bloquean;
    // revalida (que `to` siga abierta, no averiada y con lugar para todos), pasa los cupos en la
    // flota, registra un orderReassigned por pedido y los mueve al PRINCIPIO de la mochila de `to`,
    // en su orden, todo en la misma sección crítica. refused: no se movió nada (probar con otra).
    enum class Handover { done, refused, stopped };
    Handover transferTo(Courier& to);

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
    int reassigned() const { return _reassigned; }  // pedidos que esta moto traspasó

private:
    // Registra el evento salvo que se haya pedido parar o `t` ya pase el fin; false si no lo registró.
    bool logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant);

    // Un pedido en la mochila: lo asignado y si ya se recogió. Si se heredó de una moto averiada
    // después de recogido, la comida sigue allá: `fetchAt` es el punto de la avería y `foodWith` la
    // moto que la tiene (no es dueño: todas las motos viven hasta después del join).
    struct Carried {
        explicit Carried(Order assigned) : order(std::move(assigned)) {}

        Order order;
        bool picked = false;
        std::optional<Place> fetchAt;
        Courier* foodWith = nullptr;
    };
    // Lo que el hilo de la moto necesita saber del próximo pedido (una copia: se usa fuera del lock).
    struct Next {
        std::string id, restaurant, destination;
        bool picked;
        std::optional<Place> fetchAt;
    };
    // Duerme sin CPU hasta tener un pedido; std::nullopt si se abortó, o se cerró y no queda nada.
    std::optional<Next> waitForWork();
    // El pedido `id` de la mochila (lo busca por id: otros hilos pueden agregar pedidos). Con _mutex.
    std::deque<Carried>::iterator find(const std::string& id);

    // interrupted: se averió en el camino. Lo que lleva pudo cambiar (lo traspasó, o le asignaron algo
    // mientras tanto): no sigue el viaje a ciegas, vuelve a mirar su mochila.
    enum class Leg { arrived, noPath, stopped, interrupted };
    // Un tramo hasta `place` (id de restaurante o de nodo): por el camino mínimo si hay mapa, o
    // `legMs` si no. noPath: no hay camino desde donde está (solo con mapas de evaluadores).
    Leg travel(const std::string& place, bool isRestaurant);
    Leg travelTo(const Place& target);
    // Ir al punto de una avería a buscar la comida heredada y avisarle a la moto que la tenía.
    Leg rescue(const Place& spot);
    // Otra moto se llevó `count` de la comida que esta moto averiada tenía (la que espera para repararse).
    void foodCollected(int count);
    void publish(const Trip& trip);
    // ¿Se avería en este viaje? Si sí, en qué instante (entre la salida y la llegada).
    std::optional<std::int64_t> drawBreakdown(const Trip& trip);
    // La avería en `at`: quieta donde iba, fuera de la flota, y la reparación. false si se detuvo.
    bool breakDown(const Trip& trip, std::int64_t at);  // false si se detuvo
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
    bool _exited = false;  // el hilo salió de run(): ya no puede recibir un traspaso
    bool _broken = false;  // averiada: no acepta asignaciones (la flota lleva lo mismo para elegir motos)
    int _foodLeft = 0;     // comida suya que otra moto todavía tiene que venir a buscar

    int _delivered = 0;
    std::int64_t _totalDeliveryMs = 0;  // suma de (entregado - creado)
    std::int64_t _maxDeliveryMs = 0;

    const double _breakdownProbability;
    std::mt19937 _rng;  // propio de esta moto: solo lo usa su hilo (como el del generador de pedidos)
    int _breakdowns = 0;
    int _reassigned = 0;  // lo escribe solo el hilo que traspasa la mochila de esta moto
    std::function<bool(Courier&)> _handOver;  // se fija antes de lanzar el hilo; después solo se lee
};
