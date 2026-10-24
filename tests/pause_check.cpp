// Prueba de las pausas de cocina en simulaciones completas (variante B): los eventos de pausa caen en
// los límites exactos del calendario, alternan por restaurante y nunca quedan después de
// simulationStopping; ningún pickupStarted cae dentro de una pausa; pauses y pickupsDelayedByPause,
// recalculados desde el log, coinciden con el resumen; el fin natural termina (el hilo del calendario
// no deja colgado el join); y la detención en mitad de una pausa termina enseguida.
// Sale con 0 si todo está bien.
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido; r0 en b y r1 en d (1 m por ms simulado). Un puesto
// por restaurante y sin preparación: con pedidos seguidos se forman filas.
Config town(std::int64_t durationS, std::int64_t every, std::int64_t pause)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 1, 0, 0}};
    c.couriers = 3;
    c.bagCapacity = 3;
    c.speedKmh = 3600;
    c.startNode = "a";
    c.meanIntervalMs = 2000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 7;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = 100000000;
    c.breakdownProbability = 0;
    c.durationS = durationS;
    c.timeScale = 1000;  // 1 s simulado = 1 ms real
    c.kitchen = KitchenConfig{every, pause};
    return c;
}

// El número de la propiedad `key` en una línea del log (-1 si no está).
std::int64_t numberOf(const std::string& line, const std::string& key)
{
    const std::string open = "\"" + key + "\": ";
    const auto at = line.find(open);
    return at == std::string::npos ? -1 : std::stoll(line.substr(at + open.size()));
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

}  // namespace

int main()
{
    bool ok = true;
    const std::atomic<bool> never(false);

    // 1. Por tiempo: 600 s simulados, pausas de 20 s cada 60 s (r0 desde 0, r1 desde 30 s).
    {
        const Config c = town(600, 60000, 20000);
        const SimParams p = paramsFromConfig(c);
        std::ostringstream logOut, out;
        const Summary s = runSimulation(p, logOut, out, never);
        const std::vector<std::string> log = lines(logOut.str());
        std::map<std::string, PauseSchedule> schedule;
        for (std::size_t i = 0; i < c.restaurants.size(); ++i) {
            schedule[c.restaurants[i].id] = PauseSchedule::forRestaurant(60000, 20000, i, c.restaurants.size());
        }
        int paused = 0, resumed = 0, pickups = 0, waited = 0;
        bool exact = true, alternate = true, outside = true, nothingAfterStop = true, stopSeen = false;
        std::map<std::string, bool> inPause;
        std::set<std::tuple<std::string, std::string, std::int64_t>> visits;  // (moto, restaurante, t)
        std::int64_t delayed = 0;
        for (const std::string& line : log) {
            const std::string event = field(line, "event"), restaurant = field(line, "restaurant");
            const std::int64_t t = timeOf(line);
            if (stopSeen) {
                nothingAfterStop = false;
            }
            if (event == "simulationStopping") {
                stopSeen = true;
            } else if (event == "restaurantPaused" || event == "restaurantResumed") {
                const PauseSchedule& k = schedule.at(restaurant);
                const bool starts = event == "restaurantPaused";
                // Un límite exacto: en t empieza (o termina) la pausa, y en t - 1 todavía no (o sí).
                exact = exact && k.pausedAt(t) == starts && k.pausedAt(t - 1) != starts;
                alternate = alternate && inPause[restaurant] != starts;
                inPause[restaurant] = starts;
                (starts ? paused : resumed) += 1;
            } else if (event == "pickupStarted") {
                ++pickups;
                const PauseSchedule& k = schedule.at(restaurant);
                outside = outside && !k.pausedAt(t);
                const std::int64_t arrived = numberOf(line, "arrivedAt");
                // Una visita (varios pedidos del mismo restaurante) es una sola demora.
                if (visits.emplace(field(line, "courier"), restaurant, t).second) {
                    delayed += k.startsBetween(arrived, t) + (k.pausedAt(arrived) ? 1 : 0);
                    waited += arrived < t ? 1 : 0;
                }
            }
        }
        std::cout << "por tiempo: " << paused << " restaurantPaused, " << resumed << " restaurantResumed, "
                  << pickups << " pickupStarted (" << waited << " visitas con fila); resumen: pauses "
                  << s.pauses << ", pickupsDelayedByPause " << s.pickupsDelayedByPause << " (desde el log: "
                  << delayed << ")\n";
        ok &= check("por tiempo: los eventos de pausa caen en los límites exactos del calendario", exact);
        ok &= check("por tiempo: pausa y reanudación alternan por restaurante", alternate);
        ok &= check("por tiempo: 20 pausas en 600 s (10 por restaurante), ninguna después de simulationStopping",
                    paused == 20 && resumed == 20 && stopSeen && nothingAfterStop);
        ok &= check("por tiempo: ningún pickupStarted cae dentro de una pausa", pickups > 0 && outside);
        ok &= check("por tiempo: pauses del resumen = líneas restaurantPaused", s.hasPauses && s.pauses == paused);
        ok &= check("por tiempo: pickupsDelayedByPause del resumen = recalculado desde el log",
                    delayed > 0 && s.pickupsDelayedByPause == delayed);
    }

    // 2. Fin natural (tope de pedidos, sin duración): termina aunque el calendario esté dormido
    //    esperando su próxima pausa, y no se registra simulationStopping.
    {
        SimParams p = paramsFromConfig(town(0, 60000, 20000));
        p.orders = 15;
        std::ostringstream logOut, out;
        const auto start = std::chrono::steady_clock::now();
        const Summary s = runSimulation(p, logOut, out, never);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::cout << "fin natural: " << s.created << " creados, " << s.delivered << " entregados, en " << ms.count()
                  << " ms\n";
        ok &= check("fin natural: termina (el join del calendario no se cuelga) y sin simulationStopping",
                    s.created == 15 && s.delivered + s.rejected == s.created &&
                        logOut.str().find("simulationStopping") == std::string::npos);
    }

    // 3. Detención en mitad de una pausa con motos en la fila: r0 en pausa [0, 50 s) y r1 [30 s, 80 s);
    //    se pide parar a los ~40 s simulados (40 ms reales), con los dos en pausa.
    {
        const SimParams p = paramsFromConfig(town(0, 60000, 50000));
        std::atomic<bool> stop(false);
        std::chrono::steady_clock::time_point stoppedAt;  // lo escribe `stopper`; se lee después de su join
        std::thread stopper([&stop, &stoppedAt] {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));  // representa "en algún momento"
            stoppedAt = std::chrono::steady_clock::now();
            stop.store(true);
        });
        std::ostringstream logOut, out;
        const Summary s = runSimulation(p, logOut, out, stop);
        const auto returnedAt = std::chrono::steady_clock::now();
        stopper.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - stoppedAt).count();
        const std::vector<std::string> log = lines(logOut.str());
        std::cout << "parada en pausa: " << s.pauses << " pausas registradas, terminó " << ms
                  << " ms después de la orden; último evento: " << (log.empty() ? "" : log.back()) << "\n";
        ok &= check("parada en pausa: termina enseguida y simulationStopping es el último evento",
                    ms < 1000 && !log.empty() && field(log.back(), "event") == "simulationStopping");
    }
    return ok ? 0 : 1;
}
