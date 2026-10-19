// Prueba de las averías: la flota no ofrece una moto averiada; con probabilidad 1 toda moto se
// avería en cada viaje (y no entrega nada); con una probabilidad intermedia hay averías y entregas,
// y mientras una moto está averiada no hace nada (no recoge ni entrega) durante su reparación.
// El resumen cuenta exactamente las averías del log. Sale con 0 si todo está bien.
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido; r0 en b y r1 en d (1 m por ms simulado).
Config smallTown(double breakdownProbability, std::int64_t durationS)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 2, 0, 0}};
    c.couriers = 2;
    c.bagCapacity = 3;
    c.speedKmh = 3600;
    c.startNode = "a";
    c.meanIntervalMs = 5000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 7;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = 100000000;  // sin vencimiento práctico: aquí no se mide noCourier
    c.breakdownProbability = breakdownProbability;
    c.durationS = durationS;
    c.timeScale = 1000;  // 1 s simulado = 1 ms real: la reparación (5 min) dura 0,3 s reales
    return c;
}

struct Run {
    Summary summary;
    std::vector<std::string> lines;
};

Run simulate(const Config& c, int orders)
{
    SimParams p = paramsFromConfig(c);
    p.orders = orders;
    std::ostringstream logOut, out;
    Run run;
    run.summary = runSimulation(p, logOut, out);
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        run.lines.push_back(line);
    }
    return run;
}

int count(const Run& run, const std::string& event)
{
    int n = 0;
    for (const std::string& line : run.lines) {
        n += field(line, "event") == event;
    }
    return n;
}

// Mientras está averiada (de la avería a la reparación) la moto no registra nada propio: ni
// recoge ni entrega. Y dos averías de la misma moto están separadas al menos por la reparación.
bool quietWhileBroken(const Run& run)
{
    std::map<std::string, std::int64_t> brokeAt;
    for (const std::string& line : run.lines) {
        const std::string event = field(line, "event"), courier = field(line, "courier");
        const auto it = brokeAt.find(courier);
        const bool broken = it != brokeAt.end() && timeOf(line) < it->second + kRepairMs;
        if (event == "courierBreakdown") {
            if (broken) {
                return false;
            }
            brokeAt[courier] = timeOf(line);
        } else if ((event == "pickupStarted" || event == "orderPickedUp" || event == "orderDelivered") && broken) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. La flota: una moto averiada no tiene lugar (no es candidata ni se le reserva cupo).
    Fleet fleet(2, 3);
    const std::uint64_t before = fleet.version();
    fleet.setBroken(0, true);
    const bool hidden = fleet.withSpace() == std::vector<std::size_t>{1} && !fleet.tryReserve(0);
    fleet.setBroken(0, false);
    ok &= check("flota: la averiada no es candidata ni se le reserva cupo; reparada, vuelve",
                hidden && fleet.withSpace().size() == 2 && fleet.tryReserve(0) && fleet.version() != before);

    // 2. Probabilidad 1: cada viaje se avería antes de llegar, así que nadie entrega nada.
    const Run always = simulate(smallTown(1.0, 1800), 6);
    std::cout << "p = 1: " << toJson(always.summary) << "\n";
    ok &= check("p = 1: hay averías, el resumen las cuenta igual que el log, y no se entrega nada",
                always.summary.breakdowns > 0 && always.summary.breakdowns == count(always, "courierBreakdown") &&
                    always.summary.delivered == 0 && count(always, "orderAssigned") > 0);
    ok &= check("p = 1: averiada no hace nada hasta repararse (5 min simulados)", quietWhileBroken(always));

    // 3. Probabilidad 0,3 con dos motos: averías y entregas, y al final se entrega todo. Si las dos
    //    quedan averiadas a la vez, ninguna puede recibir: se reparan igual y siguen con sus pedidos
    //    (si esperaran a la otra, la simulación quedaría quieta y no terminaría).
    const Run sometimes = simulate(smallTown(0.3, 0), 30);
    std::cout << "p = 0,3: " << toJson(sometimes.summary) << "\n";
    ok &= check("p = 0,3: hay averías, contadas igual que en el log, y los 30 se entregan",
                sometimes.summary.breakdowns > 0 &&
                    sometimes.summary.breakdowns == count(sometimes, "courierBreakdown") &&
                    sometimes.summary.delivered == 30 && count(sometimes, "orderDelivered") == 30);
    ok &= check("p = 0,3: averiada no recoge ni entrega hasta repararse", quietWhileBroken(sometimes));

    // 5. Una moto averiada rechaza asignaciones. Es lo que pasaba entre el despachador y la avería: el
    //    despachador reserva el cupo, la moto se avería y traspasa todo, y recién después llega la
    //    asignación. Aquí la función de traspaso (corre en el hilo de la moto, ya averiada) traspasa la
    //    mochila y enseguida intenta asignarle otro pedido: tiene que rechazarlo (full) sin moverlo.
    {
        const SimClock clock(1000.0);
        const Shutdown shutdown;
        std::ostringstream out;
        EventLog log(out);
        const std::shared_ptr<const World> world = paramsFromConfig(smallTown(1.0, 0)).world;
        auto fleet = std::make_shared<Fleet>(2, 3);
        auto a = std::make_shared<Courier>("c0", clock, log, shutdown, 1000, world, nullptr, fleet, 0, 1.0, 1);
        auto b = std::make_shared<Courier>("c1", clock, log, shutdown, 1000, world, nullptr, fleet, 1, 0.0, 2);
        std::promise<void> brokeOnce;
        bool first = true;
        OrderBook::Submit lateResult = OrderBook::Submit::accepted;
        a->onBreakdown([&](Courier& broken) {
            if (first) {
                first = false;
                broken.transferTo(*b);
                Order late("o-late", "r0", "c", clock.nowMs());
                lateResult = broken.assign(std::move(late), [] { return true; });
                brokeOnce.set_value();
            }
            return true;
        });
        fleet->tryReserve(0);
        a->assign(Order("o1", "r0", "c", clock.nowMs()));
        std::thread worker(&Courier::run, a);
        brokeOnce.get_future().wait();  // sin girar: el hilo de la moto avisa por el promise
        a->close();
        worker.join();
        b->abort();
        ok &= check("averiada: rechaza la asignación (full) y el pedido traspasado está en la otra",
                    lateResult == OrderBook::Submit::full && a->plan().orders.empty() &&
                        b->plan().orders.size() == 1 && b->plan().orders[0].destination == "c");
    }

    // 4. Probabilidad 0: ninguna avería.
    const Run never = simulate(smallTown(0.0, 0), 10);
    ok &= check("p = 0: ninguna avería", never.summary.breakdowns == 0 && count(never, "courierBreakdown") == 0 &&
                                            never.summary.delivered == 10);
    return ok ? 0 : 1;
}
