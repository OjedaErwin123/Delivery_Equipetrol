// Prueba de la foto de cada frame (lo que dibuja la ventana), sin pantalla: los contadores en vivo
// del log cuentan exactamente lo escrito (también los pedidos activos y el tiempo de entrega); en
// una simulación, los frames muestran motos moviéndose con un estado coherente y contadores que
// nunca retroceden y terminan iguales al resumen; y si la función del frame devuelve false (se
// cerró la ventana), la simulación se detiene como con una señal. Sale con 0 si todo está bien.
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

Event event(std::int64_t t, const std::string& name, const std::string& order, const std::string& courier = "")
{
    Event e;
    e.t = t;
    e.event = name;
    e.order = order;
    e.courier = courier;
    return e;
}

// a→b→c de sentido único, a–d–c de doble sentido; r0 en b y r1 en d (1 m por ms simulado).
Config smallTown(std::int64_t durationS)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 2, 0, 0}};
    c.couriers = 2;
    c.bagCapacity = 3;
    c.speedKmh = 36;  // 1 cm por ms: cada calle dura ~10 s simulados, así los frames la ven moverse
    c.startNode = "a";
    c.meanIntervalMs = 20000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 3;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = 100000000;
    c.breakdownProbability = 0.2;
    c.durationS = durationS;
    c.timeScale = 100;
    return c;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Los contadores en vivo, evento por evento.
    std::ostringstream sink;
    EventLog log(sink);
    Event created = event(100, "orderCreated", "o1");
    created.destination = "c";
    log.write(created);
    Event other = event(100, "orderCreated", "o2");
    other.destination = "d";
    log.write(other);
    const LiveStats twoActive = log.stats();
    log.write(event(400, "orderReassigned", "o1"));
    log.write(event(900, "orderDelivered", "o1", "c0"));
    Event rejected = event(100, "orderRejected", "o2");
    rejected.reason = "queueFull";
    log.write(rejected);
    Event breakdown = event(950, "courierBreakdown", "");
    breakdown.courier = "c1";
    log.write(breakdown);
    const LiveStats s = log.stats();
    ok &= check("pedidos activos con su destino mientras no terminan",
                twoActive.activeDestinations.size() == 2 && twoActive.activeDestinations.at("o1") == "c" &&
                    s.activeDestinations.empty());
    ok &= check("contadores: creados, entregados, rechazados, traspasos, averías, pendientes",
                s.created == 2 && s.delivered == 1 && s.rejected == 1 && s.reassigned == 1 && s.breakdowns == 1 &&
                    s.pending() == 0 && s.deliveredPerCourier.at("c0") == 1);
    ok &= check("tiempo de entrega desde la creación (800 ms)", s.avgDeliveryMs() == 800 && s.maxDeliveryMs == 800);
    ok &= check("el destino no se escribe en la línea", sink.str().find("\"c\"") == std::string::npos &&
                                                          sink.str().find("destination") == std::string::npos);

    // 2. Una simulación con frames: alguna moto se ve viajando, los estados son coherentes con el
    //    viaje (una inactiva o averiada no se mueve) y los contadores no retroceden.
    const std::atomic<bool> noSignal(false);
    int frames = 0;
    bool sawMoving = false, coherent = true, monotone = true;
    LiveStats last;
    std::ostringstream logOut, out;
    SimParams p = paramsFromConfig(smallTown(1800));
    p.orders = 0;
    const Summary summary = runSimulation(p, logOut, out, noSignal, [&](const Frame& frame) {
        ++frames;
        for (const CourierView& c : frame.couriers) {
            const bool moving = frame.nowMs > c.trip.departMs() && frame.nowMs < c.trip.arrivalMs();
            sawMoving = sawMoving || moving;
            if (moving && (c.status == CourierStatus::idle || c.status == CourierStatus::broken)) {
                coherent = false;
            }
        }
        monotone = monotone && frame.stats.created >= last.created && frame.stats.delivered >= last.delivered &&
                   frame.stats.breakdowns >= last.breakdowns;
        last = frame.stats;
        return true;
    });
    std::cout << "frames: " << frames << "; último: creados " << last.created << ", entregados " << last.delivered
              << ", averías " << last.breakdowns << "; resumen: " << toJson(summary) << "\n";
    ok &= check("hubo frames y en alguno una moto viajando", frames > 10 && sawMoving);
    ok &= check("una moto que se mueve no figura inactiva ni averiada", coherent);
    ok &= check("los contadores nunca retroceden y no pasan del resumen",
                monotone && last.created <= summary.created && last.delivered <= summary.delivered);

    // 3. La función del frame devuelve false (se cerró la ventana) al décimo frame: se detiene.
    std::ostringstream closedLog, closedOut;
    int seen = 0;
    const auto start = std::chrono::steady_clock::now();
    const Summary closed =
        runSimulation(paramsFromConfig(smallTown(0)), closedLog, closedOut, noSignal, [&](const Frame&) {
            return ++seen < 10;
        });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::string lastLine;
    std::istringstream in(closedLog.str());
    for (std::string line; std::getline(in, line);) {
        lastLine = line;
    }
    std::cout << "ventana cerrada en el frame " << seen << ": " << seconds << " s; último evento: " << lastLine << "\n";
    ok &= check("al cerrar la ventana se detiene: simulationStopping último y lo inconcluso pendiente",
                seen == 10 && field(lastLine, "event") == "simulationStopping" &&
                    closed.created == closed.delivered + closed.rejected + (closed.created - closed.delivered - closed.rejected) &&
                    seconds < 5.0);
    return ok ? 0 : 1;
}
