// Prueba de los pedidos inalcanzables: a la configuración real se le agregan dos destinos a los
// que no se llega por las calles (uno sin calles y uno con una sola calle de sentido único que
// SALE de él). Los pedidos hacia ellos se crean y se rechazan con unreachable; unreachable gana
// a queueFull; y el conjunto de inalcanzables es el mismo para una semilla.
// Uso: unreachable_check <config.json>. Sale con 0 si todo está bien.
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "log_line.hpp"
#include "simulation.hpp"

namespace {

struct Run {
    Summary summary;
    std::map<std::string, std::vector<std::string>> events;  // pedido -> "evento[:motivo]@t"
};

Run simulate(const SimParams& p)
{
    std::ostringstream logOut;
    std::ostringstream out;
    Run run;
    run.summary = runSimulation(p, logOut, out);
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const std::string order = field(line, "order");
        if (!order.empty()) {
            const std::string reason = field(line, "reason");
            run.events[order].push_back(field(line, "event") + (reason.empty() ? "" : ":" + reason) + "@" +
                                        std::to_string(timeOf(line)));
        }
    }
    return run;
}

// Los pedidos rechazados con `reason`, verificando que solo tengan orderCreated + orderRejected
// con el mismo t (si no, deja `wellFormed` en false).
std::set<std::string> rejectedFor(const Run& run, const std::string& reason, bool& wellFormed)
{
    std::set<std::string> ids;
    for (const auto& order : run.events) {
        const std::vector<std::string>& ev = order.second;
        if (ev.size() == 2 && ev[1].rfind("orderRejected:" + reason + "@", 0) == 0) {
            const std::string t = ev[0].substr(ev[0].find('@'));
            wellFormed = wellFormed && ev[0] == "orderCreated" + t && ev[1] == "orderRejected:" + reason + t;
            ids.insert(order.first);
        }
    }
    return ids;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "uso: unreachable_check <config.json>\n";
        return 1;
    }
    bool ok = true;
    const Config real = loadConfig(argv[1]);

    // 1. En el mapa real todo destino es alcanzable desde todo restaurante.
    bool allReachable = true;
    for (const auto& row : paramsFromConfig(real).reachable) {
        for (bool reachable : row) {
            allReachable = allReachable && reachable;
        }
    }
    ok &= check("mapa real: todo destino alcanzable desde todo restaurante", allReachable);

    // 2. Dos destinos inalcanzables: sin calles, y con una sola calle de sentido único que sale.
    Config c = real;
    c.nodes.push_back({"aislado", real.nodes[0].lat, real.nodes[0].lon});
    c.nodes.push_back({"soloSale", real.nodes[0].lat, real.nodes[0].lon});
    c.streets.push_back({"sSalida", "soloSale", real.nodes[0].id, true});
    SimParams p = paramsFromConfig(c);
    bool tableOk = true;
    for (std::size_t d = 0; d < p.destinations.size(); ++d) {
        const bool trapped = p.destinations[d] == "aislado" || p.destinations[d] == "soloSale";
        for (std::size_t r = 0; r < p.restaurants.size(); ++r) {
            tableOk = tableOk && p.reachable[r][d] == !trapped;
        }
    }
    ok &= check("tabla: solo los dos destinos nuevos son inalcanzables (el sentido único se respeta)", tableOk);

    // 3. Una corrida rápida con tope de pedidos.
    p.orders = 3000;
    p.legMs = 1000;
    p.timeScale = 100000.0;  // 1 s simulado = 0,01 ms real
    p.durationS = 0;
    // Sin límite del libro: aquí solo se cuentan los unreachable. Con el maxPending de la
    // configuración (50), cuando todo corre lento (TSan, Valgrind) el despachador no alcanza a
    // vaciar el libro entre ráfagas y aparecen además rechazos queueFull (visto: 60-101 en vez
    // de 45). El caso con los dos motivos lo cubre el paso 5. Por lo mismo, sin vencimiento práctico
    // de la asignación: 3000 pedidos para 8 motos esperan más de acceptTimeoutMs y saldrían noCourier
    // (eso lo prueba dispatch_check). 1e12 ms y no el máximo de int64 para no desbordar los plazos.
    p.maxPending.reset();
    p.acceptTimeoutMs = 1000000000000;
    const Run run = simulate(p);
    bool wellFormed = true;
    const std::set<std::string> unreachable = rejectedFor(run, "unreachable", wellFormed);
    std::cout << "pedidos " << run.summary.created << "; inalcanzables " << unreachable.size() << " ("
              << 2 * 100.0 / static_cast<double>(p.destinations.size()) << " % de los destinos)\n"
              << "resumen: " << toJson(run.summary) << "\n";
    ok &= check("hay pedidos inalcanzables y cada uno es orderCreated + orderRejected unreachable, mismo t",
                !unreachable.empty() && wellFormed);
    ok &= check("resumen: rejected = inalcanzables; el resto entregado",
                run.summary.rejected == static_cast<int>(unreachable.size()) &&
                    run.summary.delivered + run.summary.rejected == run.summary.created);

    // 4. Reproducible: misma semilla, mismos pedidos inalcanzables.
    bool again = true;
    ok &= check("misma semilla, mismos inalcanzables", rejectedFor(simulate(p), "unreachable", again) == unreachable);

    // 5. unreachable gana a queueFull: con maxPending = 0, los mismos pedidos salen unreachable y
    //    todos los demás queueFull.
    SimParams full = p;
    full.maxPending = 0;
    const Run both = simulate(full);
    bool formed = true;
    const std::set<std::string> u = rejectedFor(both, "unreachable", formed);
    const std::set<std::string> q = rejectedFor(both, "queueFull", formed);
    ok &= check("maxPending = 0: los inalcanzables siguen siendo unreachable, el resto queueFull",
                formed && u == unreachable && u.size() + q.size() == both.events.size());
    return ok ? 0 : 1;
}
