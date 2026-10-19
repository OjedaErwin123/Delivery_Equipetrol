// Experimentos (DP.10): corre la simulación sin ventana con variantes de config/equipetrol.json, guarda
// los datos en bruto de cada corrida y escribe las tablas comparativas en Markdown. No es parte de
// delivery_sim ni de las pruebas. Desde la raíz del repositorio:
//
//   build/delivery_experiments config/equipetrol.json experiments/raw [candidatos|pool|averias|todo]
//
// Cada corrida deja en experiments/raw/<experimento>/<variante>_seed<n>/: config.json (se puede volver a
// correr con delivery_sim), params.txt (lo que no está en el formato: hilos del pool y candidatos),
// events.log y summary.json.
//
// 1. candidatos: escenario grande (64 motos, un pedido cada 1,5 s simulados): estimar con todas las motos
//    con lugar contra solo las k más cercanas (16 y 8).
// 2. pool: el mismo escenario con 1, 2, 4 y 8 hilos en el pool de estimaciones (speedup).
// 3. averias: la configuración tal cual, barriendo breakdownProbability y bagCapacity, 5 semillas.
// Todo con timeScale 600 (una hora simulada = 6 s reales). Los tiempos de ronda son tiempo REAL.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "simulation.hpp"

namespace {

using Json = nlohmann::json;

struct Result {
    Summary summary;
    double seconds = 0;
};

void makeDirs(const std::string& path)  // como mkdir -p (sin std::filesystem: g++ 7)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            ::mkdir(path.substr(0, i).c_str(), 0755);
        }
    }
}

struct Lab {
    Json base;
    std::string rawDir;  // experiments/raw

    // Una corrida: `onConfig` cambia el JSON de la configuración y `onParams` los parámetros que no
    // están en el formato (pool, candidatos). Guarda todo en rawDir/<run>/.
    Result run(const std::string& name, std::int64_t seed, const std::function<void(Json&)>& onConfig,
               const std::function<void(SimParams&)>& onParams = {}) const
    {
        const std::string dir = rawDir + "/" + name + "_seed" + std::to_string(seed);
        makeDirs(dir);
        Json j = base;
        j["orders"]["seed"] = seed;
        j["simulation"]["timeScale"] = 600;
        j["simulation"]["durationS"] = 3600;
        j["map"]["image"] = "../../../../data/equipetrol.png";  // relativa a la carpeta de la corrida
        onConfig(j);
        const std::string text = j.dump(2);
        std::ofstream(dir + "/config.json") << text << "\n";

        SimParams p = paramsFromConfig(parseConfig(text, dir));
        if (onParams) {
            onParams(p);
        }
        std::ofstream(dir + "/params.txt") << "poolThreads=" << p.poolThreads << " (0 = hardware_concurrency())\n"
                                           << "candidateLimit=" << p.candidateLimit << " (0 = todas)\n";
        std::ofstream log(dir + "/events.log");
        std::ostringstream out;
        const auto start = std::chrono::steady_clock::now();
        Result r;
        r.summary = runSimulation(p, log, out);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::ofstream(dir + "/summary.json") << toJson(r.summary) << "\n";
        return r;
    }
};

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
}

double mean(const std::vector<double>& v)
{
    return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

std::string fmt(double x, int decimals = 2)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, x);
    return buf;
}

// Las rondas y los resultados de varias semillas, juntos.
struct Pooled {
    std::vector<double> rounds;
    double delivered = 0, noCourier = 0, rejected = 0, avgMin = 0, reassigned = 0, pending = 0;
    int runs = 0;

    void add(const Result& r)
    {
        rounds.insert(rounds.end(), r.summary.estimateRoundMs.begin(), r.summary.estimateRoundMs.end());
        delivered += r.summary.delivered;
        noCourier += r.summary.noCourier;
        rejected += r.summary.rejected;
        reassigned += r.summary.reassigned;
        pending += r.summary.created - r.summary.delivered - r.summary.rejected;
        avgMin += static_cast<double>(r.summary.avgDeliveryMs) / 60000.0;
        ++runs;
    }
    double per(double total) const { return runs > 0 ? total / runs : 0; }
};

// El escenario grande: 64 motos y un pedido cada 1,5 s simulados (en promedio, con las ráfagas).
void bigFleet(Json& j)
{
    j["fleet"]["couriers"] = 64;
    j["orders"]["meanIntervalMs"] = 1500;
}

const std::int64_t kSeeds[] = {1, 2, 3};

void candidates(const Lab& lab)
{
    std::cout << "\n### Candidatos: todas las motos con lugar vs las k más cercanas\n\n"
              << "64 motos, un pedido cada 1,5 s simulados, 1 h simulada, semillas 1–3, pool por defecto.\n\n"
              << "| Candidatos | Rondas | Ronda media (ms) | p50 (ms) | p95 (ms) | Entregados | noCourier | Entrega media (min) |\n"
              << "|---|---|---|---|---|---|---|---|\n";
    for (std::size_t limit : {std::size_t(0), std::size_t(16), std::size_t(8)}) {
        const std::string name = limit == 0 ? std::string("todas") : "k" + std::to_string(limit);
        Pooled all;
        for (std::int64_t seed : kSeeds) {
            all.add(lab.run("candidatos/" + name, seed, bigFleet, [limit](SimParams& p) { p.candidateLimit = limit; }));
        }
        std::cout << "| " << (limit == 0 ? std::string("todas") : "k = " + std::to_string(limit)) << " | "
                  << all.rounds.size() << " | " << fmt(mean(all.rounds), 3) << " | " << fmt(percentile(all.rounds, 0.5), 3)
                  << " | " << fmt(percentile(all.rounds, 0.95), 3) << " | " << fmt(all.per(all.delivered), 1) << " | "
                  << fmt(all.per(all.noCourier), 1) << " | " << fmt(all.per(all.avgMin), 2) << " |\n";
    }
}

void poolSize(const Lab& lab)
{
    std::cout << "\n### Tamaño del pool de estimaciones\n\n"
              << "El mismo escenario, estimando con todas; speedup = ronda media con 1 hilo / ronda media con n.\n\n"
              << "| Hilos | Rondas | Ronda media (ms) | p95 (ms) | Speedup | Eficiencia | Entregados |\n"
              << "|---|---|---|---|---|---|---|\n";
    double one = 0;
    for (std::size_t threads : {std::size_t(1), std::size_t(2), std::size_t(4), std::size_t(8)}) {
        Pooled all;
        for (std::int64_t seed : kSeeds) {
            all.add(lab.run("pool/hilos" + std::to_string(threads), seed, bigFleet,
                            [threads](SimParams& p) { p.poolThreads = threads; }));
        }
        const double m = mean(all.rounds);
        if (threads == 1) {
            one = m;
        }
        const double speedup = m > 0 ? one / m : 0;
        std::cout << "| " << threads << " | " << all.rounds.size() << " | " << fmt(m, 3) << " | "
                  << fmt(percentile(all.rounds, 0.95), 3) << " | " << fmt(speedup) << " | "
                  << fmt(speedup / static_cast<double>(threads)) << " | " << fmt(all.per(all.delivered), 1) << " |\n";
    }
}

void breakdownsAndBag(const Lab& lab)
{
    std::cout << "\n### Averías y capacidad de la mochila\n\n"
              << "La configuración de equipetrol.json (8 motos), 1 h simulada, semillas 1–5 (promedios).\n\n"
              << "| breakdownProbability | bagCapacity | Entregados | Rechazados | Pendientes | Reasignados | Entrega media (min) |\n"
              << "|---|---|---|---|---|---|---|\n";
    const std::pair<double, std::int64_t> cases[] = {{0.0, 3}, {0.02, 3}, {0.1, 3}, {0.02, 1}, {0.02, 5}};
    for (const auto& c : cases) {
        const std::string name = "averias/p" + fmt(c.first) + "_bag" + std::to_string(c.second);
        Pooled all;
        for (std::int64_t seed = 1; seed <= 5; ++seed) {
            all.add(lab.run(name, seed, [c](Json& j) {
                j["incidents"]["breakdownProbability"] = c.first;
                j["fleet"]["bagCapacity"] = c.second;
            }));
        }
        std::cout << "| " << fmt(c.first) << " | " << c.second << " | " << fmt(all.per(all.delivered), 1) << " | "
                  << fmt(all.per(all.rejected), 1) << " | " << fmt(all.per(all.pending), 1) << " | "
                  << fmt(all.per(all.reassigned), 1) << " | " << fmt(all.per(all.avgMin), 2) << " |\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "uso: delivery_experiments <config.json> <carpeta de datos> [candidatos|pool|averias|todo]\n";
        return 2;
    }
    const std::string which = argc > 3 ? argv[3] : "todo";
    try {
        std::ifstream in(argv[1]);
        if (!in) {
            std::cerr << "delivery_experiments: no se pudo leer '" << argv[1] << "'\n";
            return 1;
        }
        Lab lab;
        lab.base = Json::parse(in);
        lab.rawDir = argv[2];
        if (which == "candidatos" || which == "todo") {
            candidates(lab);
        }
        if (which == "pool" || which == "todo") {
            poolSize(lab);
        }
        if (which == "averias" || which == "todo") {
            breakdownsAndBag(lab);
        }
    } catch (const ConfigError& e) {
        std::cerr << "delivery_experiments: configuración inválida: " << e.what() << "\n";
        return 1;
    } catch (const Json::exception& e) {
        std::cerr << "delivery_experiments: JSON inválido: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
