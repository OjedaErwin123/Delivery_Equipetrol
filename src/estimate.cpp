#include "estimate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "restaurant.hpp"

namespace {

// Cuánto tarda el recorrido más corto de `from` (un nodo o un punto de una calle) a `to`, igual que
// un Trip: metros / velocidad, hacia arriba.
std::int64_t legMs(const World& world, const Place& from, const Place& to)
{
    const std::optional<Route> route = routeTo(world.graph, from, to);
    if (!route) {
        throw EstimateError("sin camino de " + describe(world.graph, from) + " a " + describe(world.graph, to));
    }
    return static_cast<std::int64_t>(std::ceil(route->meters / world.metersPerMs));
}

std::size_t nodeOf(const World& world, const std::string& place, bool isRestaurant)
{
    if (isRestaurant) {
        const auto it = world.restaurantNode.find(place);
        if (it == world.restaurantNode.end()) {
            throw EstimateError("restaurante desconocido: " + place);
        }
        return it->second;
    }
    const std::optional<std::size_t> node = world.graph.nodeIndex(place);
    if (!node) {
        throw EstimateError("destino desconocido: " + place);
    }
    return *node;
}

}  // namespace

std::int64_t estimateDelivery(const World& world, const CourierPlan& plan, const PlannedOrder& order,
                              std::int64_t nowMs)
{
    // Lo mismo que hará la moto (Courier::run y pickUp), sobre una copia de su mochila más el pedido nuevo.
    std::vector<PlannedOrder> bag = plan.orders;
    bag.push_back(order);
    const std::size_t target = bag.size() - 1;  // el pedido que se estima: el último en la fila FIFO

    // Desde el final del tramo actual: la moto no se desvía a mitad de una calle.
    Place node = plan.trip.to();
    std::int64_t t = std::max(nowMs, plan.trip.arrivalMs());

    for (std::size_t next = 0; next < bag.size(); ++next) {
        if (!bag[next].picked) {
            const std::string restaurant = bag[next].restaurant;
            const std::size_t at = nodeOf(world, restaurant, true);
            t += legMs(world, node, at);
            node = at;
            // Una visita: todos los de este restaurante que falta recoger; espera la comida del último.
            std::int64_t ready = t;
            for (std::size_t i = next; i < bag.size(); ++i) {
                if (!bag[i].picked && bag[i].restaurant == restaurant) {
                    ready = std::max(ready, bag[i].readyAtMs);
                    bag[i].picked = true;
                }
            }
            t = ready + kHandoverMs;
        } else if (bag[next].fetchAt) {
            // Heredado de una moto averiada: ir a buscar la comida (toda la de ese punto, de una vez).
            const Place spot = *bag[next].fetchAt;
            t += legMs(world, node, spot) + kRescueMs;
            node = spot;
            for (std::size_t i = next; i < bag.size(); ++i) {
                if (bag[i].fetchAt && *bag[i].fetchAt == spot) {
                    bag[i].fetchAt.reset();
                }
            }
        }
        const std::size_t destination = nodeOf(world, bag[next].destination, false);
        t += legMs(world, node, destination);
        node = destination;
        if (next == target) {
            return t;
        }
    }
    return t;  // no se llega: el pedido estimado siempre es el último
}
