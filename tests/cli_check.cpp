// Prueba de los argumentos de línea de comandos y de los parámetros que salen de la
// configuración. Uso: cli_check <config.json>. Sale con 0 si todo está bien.
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "cli.hpp"
#include "config.hpp"
#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// true si parseArgs rechaza `args` con un mensaje que contiene `expected`.
bool rejects(const std::vector<std::string>& args, const std::string& expected)
{
    try {
        parseArgs(args);
    } catch (const CliError& e) {
        const std::string message = e.what();
        std::cout << "  -> \"" << message << "\"\n";
        return message.find(expected) != std::string::npos;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    bool ok = true;

    // 1. Argumentos válidos.
    const CliArgs onlyConfig = parseArgs({"config/equipetrol.json"});
    ok &= check("solo la configuración: log por defecto events.log",
                onlyConfig.configPath == "config/equipetrol.json" && onlyConfig.logPath == "events.log");
    const CliArgs withLog = parseArgs({"config/equipetrol.json", "--log", "run1.log"});
    ok &= check("con --log run1.log", withLog.configPath == "config/equipetrol.json" && withLog.logPath == "run1.log");
    ok &= check("sin banderas de EC2: con ventana y simulando",
                !onlyConfig.headless && !onlyConfig.bench() && onlyConfig.benchPath.empty());
    const CliArgs bench = parseArgs({"c.json", "--headless", "--bench", "r.csv"});
    ok &= check("--headless --bench r.csv", bench.headless && bench.bench() && bench.benchPath == "r.csv" &&
                                                 !bench.benchOnce && bench.logPath == "events.log");
    const CliArgs once = parseArgs({"c.json", "--bench-once", "--log", "x.log", "--headless"});
    ok &= check("--bench-once sin --bench: mide sin CSV; las banderas en cualquier orden",
                once.bench() && once.benchOnce && once.benchPath.empty() && once.headless && once.logPath == "x.log");
    const CliArgs one = parseArgs({"c.json", "--bench-once", "--bench-app", "t1_apsp"});
    ok &= check("--bench-once --bench-app t1_apsp", one.bench() && one.benchApp == "t1_apsp");
    const CliArgs report = parseArgs({"c.json", "--headless", "--report", "r.json"});
    ok &= check("--report r.json (simula, y después el reporte)", !report.bench() && report.reportPath == "r.json");

    // 2. Argumentos inválidos: el enunciado solo define la configuración y --log.
    ok &= check("sin argumentos", rejects({}, "falta el archivo de configuración"));
    ok &= check("bandera antes de la configuración", rejects({"--log", "x.log"}, "primer argumento"));
    ok &= check("bandera desconocida", rejects({"c.json", "--lgo", "x.log"}, "argumento desconocido '--lgo'"));
    ok &= check("--log sin ruta", rejects({"c.json", "--log"}, "--log necesita"));
    ok &= check("--log seguido de otra bandera", rejects({"c.json", "--log", "--x"}, "--log necesita"));
    ok &= check("--log dos veces", rejects({"c.json", "--log", "a.log", "--log", "b.log"}, "más de una vez"));
    ok &= check("argumento de más", rejects({"c.json", "otro.json"}, "argumento desconocido 'otro.json'"));
    ok &= check("--bench sin ruta", rejects({"c.json", "--bench"}, "--bench necesita"));
    ok &= check("--headless dos veces", rejects({"c.json", "--headless", "--headless"}, "más de una vez"));
    ok &= check("--bench-app sin --bench", rejects({"c.json", "--bench-app", "t1_apsp"}, "solo sirve con"));
    ok &= check("--report sin ruta", rejects({"c.json", "--report"}, "--report necesita"));
    ok &= check("--report con --bench", rejects({"c.json", "--report", "r.json", "--bench-once"}, "no se combina"));
    ok &= check("--bench-app sin nombre", rejects({"c.json", "--bench-once", "--bench-app"}, "--bench-app necesita"));
    ok &= check("--bench-once con valor de más", rejects({"c.json", "--bench-once", "x"}, "argumento desconocido 'x'"));

    // 3. Los parámetros que salen de la configuración real.
    if (argc < 2) {
        std::cerr << "uso: cli_check <config.json>\n";
        return 1;
    }
    const Config c = loadConfig(argv[1]);
    const SimParams p = paramsFromConfig(c);
    ok &= check("motos, semilla, intervalo, duración y escala de la configuración",
                p.couriers == c.couriers && p.seed == static_cast<std::uint32_t>(c.seed) &&
                    p.meanIntervalMs == c.meanIntervalMs && p.durationS == c.durationS && p.timeScale == c.timeScale);
    ok &= check("sin tope de pedidos (se crean hasta la detención)", p.orders == 0);
    bool sameRestaurants = p.restaurants.size() == c.restaurants.size();
    std::set<std::string> restaurantNodes;
    for (std::size_t i = 0; i < c.restaurants.size(); ++i) {
        sameRestaurants = sameRestaurants && p.restaurants[i] == c.restaurants[i].id;
        restaurantNodes.insert(c.restaurants[i].node);
    }
    ok &= check("los restaurantes de la configuración", sameRestaurants);
    const bool noRestaurantNode = std::none_of(p.destinations.begin(), p.destinations.end(),
                                               [&](const std::string& n) { return restaurantNodes.count(n) > 0; });
    std::cout << "destinos posibles: " << p.destinations.size() << " de " << c.nodes.size() << " nodos ("
              << restaurantNodes.size() << " son de restaurantes)\n";
    ok &= check("destinos: todos los nodos menos los de restaurantes",
                noRestaurantNode && p.destinations.size() == c.nodes.size() - restaurantNodes.size());

    // 4. Una configuración válida donde todos los nodos son de restaurantes: igual hay destinos.
    Config tiny = c;
    tiny.nodes = {{"n0", -17.76, -63.19}};
    tiny.streets.clear();  // válida: sin calles que apunten a nodos que ya no están
    tiny.restaurants = {{"r0", "R0", "n0", 1, 0, 0}};
    const SimParams q = paramsFromConfig(tiny);
    ok &= check("si todos los nodos son de restaurantes, cualquier nodo sirve de destino",
                q.destinations == std::vector<std::string>{"n0"});
    return ok ? 0 : 1;
}
