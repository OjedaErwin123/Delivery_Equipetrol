#include "courier.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

Courier::Courier(std::string id, const SimClock& clock, EventLog& log, const Shutdown& shutdown,
                 std::int64_t legMs, std::shared_ptr<const World> world,
                 std::shared_ptr<const Restaurants> restaurants, std::shared_ptr<Fleet> fleet, std::size_t index,
                 double breakdownProbability, std::uint32_t seed)
    : _id(std::move(id)), _clock(clock), _log(log), _shutdown(shutdown), _legMs(legMs), _world(std::move(world)),
      _restaurants(std::move(restaurants)), _fleet(std::move(fleet)), _index(index),
      _at(_world ? _world->startNode : 0), _trip(Trip::stay(_at, 0)),
      _breakdownProbability(breakdownProbability), _rng(seed)
{
}

void Courier::finished()
{
    if (_fleet) {
        _fleet->release(_index);
    }
}

Trip Courier::trip() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _trip;
}

CourierPlan Courier::plan() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    CourierPlan plan(_trip);
    for (const Carried& c : _bag) {
        plan.orders.push_back({c.order.restaurant(), c.order.destination(), c.order.readyAtMs(), c.picked, c.fetchAt});
    }
    return plan;
}

OrderBook::Submit Courier::assign(Order&& order, const std::function<bool()>& logAssigned)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_closed || _aborted) {
            return OrderBook::Submit::closed;
        }
        if (_broken) {
            return OrderBook::Submit::full;  // se averió después de que le reservaran el cupo
        }
        if (!logAssigned()) {
            return OrderBook::Submit::closed;  // se detuvo
        }
        _bag.emplace_back(std::move(order));  // con su evento, en la misma sección crítica
    }
    _work.notify_one();
    return OrderBook::Submit::accepted;
}

OrderBook::Submit Courier::assign(Order&& order)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_closed || _aborted) {
            return OrderBook::Submit::closed;
        }
        _bag.emplace_back(std::move(order));
    }
    _work.notify_one();  // solo el hilo de la moto espera en _work
    return OrderBook::Submit::accepted;
}

void Courier::close()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _closed = true;
    }
    _work.notify_all();
}

void Courier::abort()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _aborted = true;
    }
    _work.notify_all();
}

std::optional<Courier::Next> Courier::waitForWork()
{
    std::unique_lock<std::mutex> lock(_mutex);
    // Predicado: despertares espurios, y el cierre o la detención (07).
    _work.wait(lock, [this] { return _aborted || _closed || !_bag.empty(); });
    if (_aborted || _bag.empty()) {
        _exited = true;  // con el mismo lock: un traspaso posterior la ve y busca otra moto
        return std::nullopt;
    }
    const Carried& first = _bag.front();
    return Next{first.order.id(), first.order.restaurant(), first.order.destination(), first.picked, first.fetchAt};
}

std::deque<Courier::Carried>::iterator Courier::find(const std::string& id)
{
    return std::find_if(_bag.begin(), _bag.end(), [&id](const Carried& c) { return c.order.id() == id; });
}

Courier::Handover Courier::transferTo(Courier& to)
{
    if (&to == this) {
        return Handover::refused;
    }
    bool stopped = false;
    {
        // Orden de locks: las dos motos (juntas), después la flota, después Shutdown y el log.
        std::scoped_lock both(_mutex, to._mutex);
        if (_bag.empty()) {
            return Handover::done;
        }
        if (to._aborted || to._exited) {
            return Handover::refused;  // cerrada no importa: solo dice que el despachador no le asigna más
        }
        const std::int64_t t = _clock.nowMs();
        const auto commit = [&] {
            // Los eventos de todos los pedidos en UNA llamada: o se registran todos, o ninguno.
            const bool logged = _shutdown.unlessStopped(t, [&] {
                for (const Carried& c : _bag) {
                    Event e;
                    e.t = t;
                    e.event = "orderReassigned";
                    e.order = c.order.id();
                    e.from = _id;
                    e.to = to._id;
                    _log.write(e);
                }
            });
            if (!logged) {
                stopped = true;
                return false;
            }
            // La comida ya recogida se queda aquí, en el punto de la avería: el receptor la viene a buscar.
            for (Carried& c : _bag) {
                if (c.picked && !c.fetchAt) {
                    c.fetchAt = _trip.to();
                    c.foodWith = this;
                    ++_foodLeft;
                }
            }
            for (auto it = _bag.rbegin(); it != _bag.rend(); ++it) {
                to._bag.push_front(std::move(*it));  // al principio, en el mismo orden (los Order se mueven: 02)
            }
            _reassigned += static_cast<int>(_bag.size());
            _bag.clear();
            return true;
        };
        const bool moved = _fleet ? _fleet->moveAssigned(_index, to._index, _bag.size(), commit) : commit();
        if (!moved) {
            return stopped ? Handover::stopped : Handover::refused;
        }
    }
    to._work.notify_one();  // por si estaba esperando trabajo
    return Handover::done;
}

void Courier::publish(const Trip& trip)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _trip = trip;
}

void Courier::foodCollected(int count)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _foodLeft -= count;
    }
    _work.notify_all();  // la moto averiada espera en _work a que le retiren toda la comida
}

Courier::Leg Courier::travel(const std::string& place, bool isRestaurant)
{
    if (!_world) {
        return _shutdown.sleepUntil(_clock.realTimeIn(_legMs)) ? Leg::stopped : Leg::arrived;
    }
    return travelTo(isRestaurant ? _world->restaurantNode.at(place) : *_world->graph.nodeIndex(place));
}

Courier::Leg Courier::rescue(const Place& spot)
{
    const Leg leg = travelTo(spot);
    if (leg != Leg::arrived) {
        return leg;
    }
    // Pasar la comida de una moto a la otra: tiempo simulado, interrumpible.
    if (_shutdown.sleepUntil(_clock.realTimeIn(kRescueMs))) {
        return Leg::stopped;
    }
    std::vector<Courier*> owners;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (Carried& c : _bag) {
            if (c.fetchAt && *c.fetchAt == spot) {  // toda la comida de ese punto, de una vez
                c.fetchAt.reset();
                owners.push_back(c.foodWith);
                c.foodWith = nullptr;
            }
        }
    }
    for (Courier* owner : owners) {
        owner->foodCollected(1);  // fuera del lock de esta moto: nunca dos mutex de moto a la vez aquí
    }
    return Leg::arrived;
}

Courier::Leg Courier::travelTo(const Place& target)
{
    if (!_world) {
        return _shutdown.sleepUntil(_clock.realTimeIn(_legMs)) ? Leg::stopped : Leg::arrived;
    }
    for (;;) {  // cada vuelta es un viaje: si se avería, se repara y sale de nuevo desde donde quedó
        const std::optional<Route> route = routeTo(_world->graph, _at, target);
        if (!route) {
            return Leg::noPath;
        }
        // Se publica el viaje una vez y se duerme hasta la llegada: la posición en cada instante
        // sale del Trip (función del tiempo), la moto no despierta para "avanzar". Si se va a
        // averiar, se publica ya cortado en ese instante: nadie la ve pasar del punto de la avería.
        const Trip trip(_world->graph, *route, _clock.nowMs(), _world->metersPerMs);
        const std::optional<std::int64_t> breaksAt = drawBreakdown(trip);
        publish(breaksAt ? trip.until(*breaksAt) : trip);
        if (_shutdown.sleepUntil(_clock.realTimeAt(breaksAt ? *breaksAt : trip.arrivalMs()))) {
            return Leg::stopped;
        }
        if (!breaksAt) {
            _at = target;
            publish(Trip::stay(_at, trip.arrivalMs()));
            return Leg::arrived;
        }
        // Después de una avería no se sigue el viaje a ciegas: la mochila pudo cambiar.
        return breakDown(trip, *breaksAt) ? Leg::interrupted : Leg::stopped;
    }
}

std::optional<std::int64_t> Courier::drawBreakdown(const Trip& trip)
{
    const std::int64_t duration = trip.arrivalMs() - trip.departMs();
    if (_breakdownProbability <= 0 || duration <= 0) {
        return std::nullopt;  // un tramo de largo cero no es un viaje
    }
    // Directo del mt19937 (no uniform_*_distribution: da valores distintos en libstdc++ y libc++).
    const double u = static_cast<double>(_rng()) / 4294967296.0;  // en [0, 1)
    if (u >= _breakdownProbability) {
        return std::nullopt;
    }
    return trip.departMs() + static_cast<std::int64_t>(static_cast<std::uint64_t>(_rng()) %
                                                       static_cast<std::uint64_t>(duration));
}

bool Courier::breakDown(const Trip& trip, std::int64_t at)
{
    _at = trip.placeAt(at);  // a mitad de una calle (o en un nodo, si justo pasaba por uno)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _trip = Trip::stay(_at, at);
        _broken = true;  // desde aquí rechaza asignaciones (con el mismo lock con que se asigna)
    }
    if (_fleet) {
        _fleet->setBroken(_index, true);  // ya no es candidata del despachador
    }
    Event e;
    e.t = at;
    e.event = "courierBreakdown";
    e.courier = _id;
    if (!_shutdown.unlessStopped(at, [this, &e] { _log.write(e); })) {
        return false;
    }
    ++_breakdowns;  // el mismo evento que se registró: el resumen coincide con el log
    // Traspasa toda la mochila (espera receptor si hace falta). La reparación empieza después.
    if (_handOver && !_handOver(*this)) {
        return false;
    }
    {
        // La reparación empieza cuando le retiraron toda la comida que ya había recogido (sin CPU).
        std::unique_lock<std::mutex> lock(_mutex);
        _work.wait(lock, [this] { return _aborted || _foodLeft == 0; });
        if (_aborted) {
            return false;
        }
    }
    // La reparación: tiempo simulado, interrumpible por la detención y sin CPU.
    if (_shutdown.sleepUntil(_clock.realTimeIn(kRepairMs))) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _broken = false;
    }
    if (_fleet) {
        _fleet->setBroken(_index, false);
    }
    return true;
}

Courier::Leg Courier::pickUp(const std::string& restaurant)
{
    // Sin camino hasta el restaurante (moto atrapada, solo con mapas de evaluadores): noPath.
    const Leg leg = travel(restaurant, true);
    if (leg != Leg::arrived) {
        return leg;
    }
    // Fila (ticket) hasta tomar un puesto; nullopt = se detuvo mientras esperaba. El Slot libera el
    // puesto al destruirse (RAII), en cualquier salida. (Slot es move-only sin asignación: el
    // optional se construye directamente con el resultado.)
    std::optional<Restaurant::Slot> slot =
        _restaurants ? _restaurants->at(restaurant)->enter() : std::optional<Restaurant::Slot>();
    if (_restaurants && !slot) {
        return Leg::stopped;
    }
    // Una sola visita: todos los pedidos de la mochila de este restaurante que falta recoger,
    // también los asignados mientras viajaba o hacía fila. Un evento negado (se pidió parar, o su
    // t ya pasó el fin) corta: los pedidos quedan pendientes.
    std::vector<std::string> here;
    const std::int64_t startedAt = _clock.nowMs();
    std::int64_t readyAt = startedAt;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const Carried& c : _bag) {
            if (!c.picked && c.order.restaurant() == restaurant) {
                if (!logEvent("pickupStarted", c.order, startedAt, true)) {
                    return Leg::stopped;
                }
                here.push_back(c.order.id());
                readyAt = std::max(readyAt, c.order.readyAtMs());
            }
        }
    }
    // Con el puesto tomado: espera la comida del último si no está lista, y la entrega dura kHandoverMs.
    if (_restaurants && _shutdown.sleepUntil(_clock.realTimeAt(readyAt + kHandoverMs))) {
        return Leg::stopped;
    }
    const std::int64_t pickedAt = _clock.nowMs();
    std::lock_guard<std::mutex> lock(_mutex);
    for (const std::string& id : here) {
        const auto it = find(id);  // sigue ahí: solo esta moto saca pedidos de su mochila
        if (!logEvent("orderPickedUp", it->order, pickedAt, true)) {
            return Leg::stopped;
        }
        it->picked = true;  // con el evento, en la misma sección crítica
    }
    return Leg::arrived;  // aquí se destruye el Slot: entra el ticket siguiente
}

bool Courier::deliver(const std::string& id)
{
    // El mismo t va al log y a las estadísticas: el resumen debe coincidir con el log.
    const std::int64_t deliveredAt = _clock.nowMs();
    std::int64_t took = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = find(id);
        if (!logEvent("orderDelivered", it->order, deliveredAt, false)) {
            return false;
        }
        took = deliveredAt - it->order.createdAtMs();
        _bag.erase(it);  // con el evento: nadie lo ve entregado y todavía en la mochila
    }
    ++_delivered;
    _totalDeliveryMs += took;
    _maxDeliveryMs = std::max(_maxDeliveryMs, took);
    finished();  // después de registrar la entrega: la próxima asignación tiene un t posterior
    return true;
}

void Courier::drop(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _bag.erase(find(id));
    }
    finished();  // no puede tomarlo: queda pendiente y la moto sigue con el siguiente
}

void Courier::run()
{
    for (;;) {
        // El primero de la mochila es el próximo a entregar (FIFO). Sin pedidos, duerme sin CPU.
        const std::optional<Next> next = waitForWork();
        if (!next) {
            break;
        }
        if (next->picked && next->fetchAt) {
            // Heredado de una moto averiada: a buscar la comida al punto de la avería.
            const Leg leg = rescue(*next->fetchAt);
            if (leg == Leg::stopped) {
                break;
            }
            if (leg == Leg::noPath) {
                drop(next->id);  // solo con mapas raros: queda pendiente
            }
            continue;
        }
        if (!next->picked) {
            // Falta recogerlo: a su restaurante. Después vuelve a mirar la mochila desde el principio.
            const Leg leg = pickUp(next->restaurant);
            if (leg == Leg::stopped) {
                break;
            }
            if (leg == Leg::noPath) {
                drop(next->id);
            }
            continue;
        }
        // Al cliente hay camino desde el restaurante (si no, el pedido habría salido unreachable).
        const Leg toCustomer = travel(next->destination, false);
        if (toCustomer == Leg::stopped) {
            break;
        }
        if (toCustomer == Leg::interrupted) {
            continue;  // se averió en el camino: vuelve a mirar su mochila
        }
        if (toCustomer == Leg::noPath) {
            drop(next->id);  // no debería pasar; si pasa, el pedido queda pendiente y la moto sigue
            continue;
        }
        if (!deliver(next->id)) {
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _exited = true;
    }
    if (_fleet) {
        _fleet->retire(_index);  // una averiada que esperaba receptor vuelve a mirar
    }
}

bool Courier::logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant)
{
    Event e;
    e.t = t;
    e.event = event;
    e.order = order.id();
    e.courier = _id;
    if (withRestaurant) {
        e.restaurant = order.restaurant();
    }
    return _shutdown.unlessStopped(t, [this, &e] { _log.write(e); });
}
