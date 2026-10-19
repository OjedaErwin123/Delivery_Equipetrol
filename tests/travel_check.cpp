// Prueba del movimiento por las calles: la posición de un viaje avanza de forma continua por
// las calles del camino (respetando el sentido único), cada viaje dura metros / velocidad, la
// moto queda quieta donde entregó, y una moto sin camino al restaurante deja el pedido.
// Sale con 0 si todo está bien.
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"
#include "travel.hpp"

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// a→b→c de sentido único (~222 m, recto sobre lat 0); a–d–c de doble sentido (~314 m, por lat 0,001);
// e sin calles. El restaurante r0 está en b.
Config map()
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}, {"e", 0.002, 0.002}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}};
    return c;
}

std::shared_ptr<const World> world(const Config& c, const std::string& start)
{
    auto w = std::make_shared<World>(World{RoadGraph(c), {}, 0, 1.0});  // 1 m por ms simulado
    w->startNode = *w->graph.nodeIndex(start);
    w->restaurantNode.emplace("r0", *w->graph.nodeIndex("b"));
    return w;
}

}  // namespace

int main()
{
    bool ok = true;
    const Config c = map();
    const RoadGraph g(c);
    const std::size_t a = *g.nodeIndex("a"), cc = *g.nodeIndex("c");
    const double ab = haversineMeters(0, 0, 0, 0.001);  // ~111,2 m

    // 1. Un viaje a→c (por b) saliendo en t = 1000 a 1 m/ms.
    const Trip ac(g, *g.shortestPath(a, cc), 1000, 1.0);
    ok &= check("dura metros / velocidad (redondeado hacia arriba)",
                ac.arrivalMs() == 1000 + static_cast<std::int64_t>(std::ceil(2 * ab)));
    bool continuous = true;
    double previousLon = -1;
    for (std::int64_t t = 1000; t <= ac.arrivalMs(); t += 7) {
        const Coordinates p = ac.positionAt(g, t);
        continuous = continuous && near(p.lat, 0) && p.lon > previousLon - 1e-12 && p.lon <= 0.002 + 1e-12;
        previousLon = p.lon;
    }
    ok &= check("avanza de forma continua y nunca sale de la calle (lat 0, lon creciente)", continuous);
    const Coordinates mid = ac.positionAt(g, 1000 + static_cast<std::int64_t>(ab / 2));
    ok &= check("a mitad de la primera calle, a mitad de camino entre a y b",
                near(mid.lat, 0) && std::fabs(mid.lon - 0.0005) < 0.00001);
    ok &= check("antes de salir, en a; después de llegar, en c",
                near(ac.positionAt(g, 0).lon, 0) && near(ac.positionAt(g, ac.arrivalMs() + 100).lon, 0.002));

    // 2. c→a no puede usar a→b→c al revés (sentido único): pasa por d (lat 0,001).
    const Trip ca(g, *g.shortestPath(cc, a), 0, 1.0);
    const Coordinates half = ca.positionAt(g, (ca.arrivalMs()) / 2);
    ok &= check("c→a respeta el sentido único: va por d, no por b", half.lat > 0.0009);

    // 3. Una moto con mapa: sale de a, va al restaurante (b), entrega en c y queda quieta ahí.
    //    Reloj lento a propósito: cada calle dura ~11 ms reales, así el hilo de la prueba alcanza a
    //    verla viajando aunque todo corra lento (Valgrind ejecuta los hilos de a uno).
    const SimClock clock(10.0);
    const Shutdown shutdown;
    std::ostringstream out;
    EventLog log(out);
    auto courier = std::make_shared<Courier>("c0", clock, log, shutdown, 60000, world(c, "a"));
    std::thread worker(&Courier::run, courier);
    const std::int64_t createdAt = clock.nowMs();
    if (courier->assign(Order("o0", "r0", "c", createdAt)) != OrderBook::Submit::accepted) {
        ok = check("bandeja abierta", false);
    }
    // Mientras viaja, otro hilo lee su viaje (lo que hará la ventana): TSan vigila este acceso. Se
    // lee hasta verla quieta en el destino (con un tope de 10 s reales), no un número fijo de veces:
    // con un número fijo, bajo Valgrind las lecturas se acababan antes de que la moto saliera.
    bool sawTravelling = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const Trip t = courier->trip();
        sawTravelling = sawTravelling || t.from() != t.to();
        if ((t.from() == cc && t.to() == cc) || std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::yield();
    }
    courier->close();
    worker.join();
    std::int64_t pickedAt = -1, deliveredAt = -1;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        if (field(line, "event") == "orderPickedUp") pickedAt = timeOf(line);
        if (field(line, "event") == "orderDelivered") deliveredAt = timeOf(line);
    }
    std::cout << "recogido en t=" << pickedAt << ", entregado en t=" << deliveredAt << " (cada calle ~" << ab
              << " m a 1 m/ms)\n";
    ok &= check("otro hilo ve a la moto viajando (su Trip publicado)", sawTravelling);
    // -1: el reloj trunca a ms enteros al pasar a tiempo real.
    ok &= check("cada tramo dura al menos metros / velocidad",
                pickedAt - createdAt >= static_cast<std::int64_t>(ab) - 1 &&
                    deliveredAt - pickedAt >= static_cast<std::int64_t>(ab) - 1);
    const Trip last = courier->trip();
    ok &= check("entregó y quedó quieta en el destino (c)", courier->delivered() == 1 && last.from() == cc &&
                                                             last.to() == cc && near(last.positionAt(g, 1e9).lon, 0.002));

    // 4. Una moto atrapada (sale de e, sin calles): no puede llegar al restaurante, deja el pedido.
    std::ostringstream stuckOut;
    EventLog stuckLog(stuckOut);
    auto stuck = std::make_shared<Courier>("c1", clock, stuckLog, shutdown, 60000, world(c, "e"));
    std::thread stuckWorker(&Courier::run, stuck);
    if (stuck->assign(Order("o1", "r0", "c", clock.nowMs())) != OrderBook::Submit::accepted) {
        ok = check("bandeja abierta", false);
    }
    stuck->close();
    stuckWorker.join();  // si se quedara esperando un camino que no existe, esto se colgaría
    ok &= check("moto atrapada: deja el pedido (sin eventos, pendiente) y termina",
                stuck->delivered() == 0 && stuckOut.str().empty());
    return ok ? 0 : 1;
}
