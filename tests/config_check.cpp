// Prueba de la lectura de la configuración: el archivo real se lee completo, y cada
// configuración rota (archivo inexistente, JSON mal formado, propiedad ausente, tipo
// equivocado) se rechaza con un ConfigError que nombra la propiedad. Sale con 0 si todo
// está bien. Uso: config_check <ruta a config/equipetrol.json>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "log_line.hpp"

using nlohmann::json;

namespace {

// La configuración válida más chica: un nodo, sin calles, un restaurante.
json minimal()
{
    return json::parse(R"({
      "map": {"image": "../data/equipetrol.png", "attribution": "© OpenStreetMap contributors",
              "bounds": {"north": 1.0, "south": 0.0, "west": 0.0, "east": 1.0}},
      "nodes": [{"id": "n0", "lat": 0.5, "lon": 0.5}],
      "streets": [],
      "restaurants": [{"id": "r0", "name": "R", "node": "n0", "pickupSlots": 1, "prepTimeMs": [1000, 2000]}],
      "fleet": {"couriers": 1, "bagCapacity": 1, "speedKmh": 30, "startNode": "n0"},
      "orders": {"meanIntervalMs": 1000, "burstMax": 1, "maxPending": 10, "seed": 1},
      "dispatch": {"quoteTimeoutMs": 200, "acceptTimeoutMs": 60000},
      "incidents": {"breakdownProbability": 0.0},
      "simulation": {"durationS": 60, "timeScale": 60}
    })");
}

// Aplica `breakIt` a la configuración mínima y verifica que se rechace con un mensaje que contenga `expected`.
bool rejects(const char* name, const std::function<void(json&)>& breakIt, const std::string& expected)
{
    json j = minimal();
    breakIt(j);
    try {
        parseConfig(j.dump(), "config");
    } catch (const ConfigError& e) {
        const bool ok = std::string(e.what()).find(expected) != std::string::npos;
        std::cout << "  " << name << " -> \"" << e.what() << "\"\n";
        return check(name, ok);
    }
    return check(name, false);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "uso: config_check <ruta a config/equipetrol.json>\n";
        return 2;
    }
    bool ok = true;

    // 1. El archivo real se lee completo y la imagen se resuelve relativa a su carpeta.
    const Config c = loadConfig(argv[1]);
    std::cout << "config real: " << c.nodes.size() << " nodos, " << c.streets.size() << " calles, "
              << c.restaurants.size() << " restaurantes, imagen " << c.mapImage << "\n";
    ok &= check("archivo real: nodos, calles y restaurantes",
                c.nodes.size() == 154 && c.streets.size() == 219 && c.restaurants.size() == 7);
    ok &= check("archivo real: restaurantes y flota", c.restaurants[0].name == "Pizza Hut" &&
                                                         c.restaurants[0].pickupSlots == 1 &&
                                                         c.restaurants[0].prepMaxMs == 420000 && c.startNode == "n0");
    ok &= check("archivo real: la imagen existe en la ruta resuelta", std::ifstream(c.mapImage).good());
    ok &= check("configuración mínima: se lee", parseConfig(minimal().dump(), "config").nodes.size() == 1);

    // 2. Cada configuración inválida se rechaza con un mensaje que nombra el problema.
    try {
        loadConfig("no-existe.json");
        ok &= check("archivo inexistente", false);
    } catch (const ConfigError& e) {
        std::cout << "  archivo inexistente -> \"" << e.what() << "\"\n";
        ok &= check("archivo inexistente", std::string(e.what()).find("no se pudo abrir") != std::string::npos);
    }
    try {
        parseConfig("{ \"map\": ", "config");
        ok &= check("JSON mal formado", false);
    } catch (const ConfigError& e) {
        ok &= check("JSON mal formado", std::string(e.what()).find("mal formado") != std::string::npos);
    }
    ok &= rejects("la raíz no es un objeto", [](json& j) { j = json::array(); }, "debe ser un objeto JSON");
    ok &= rejects("falta un campo", [](json& j) { j["fleet"].erase("couriers"); }, "fleet.couriers");
    ok &= rejects("entero con decimales", [](json& j) { j["fleet"]["couriers"] = 3.5; }, "fleet.couriers: debe ser un número entero");
    ok &= rejects("falta una sección", [](json& j) { j.erase("fleet"); }, "falta la propiedad obligatoria fleet");
    ok &= rejects("falta un campo anidado", [](json& j) { j["map"]["bounds"].erase("east"); }, "map.bounds.east");
    ok &= rejects("falta un campo de un nodo", [](json& j) { j["nodes"][0].erase("lat"); }, "nodes[0].lat");
    ok &= rejects("número escrito como texto", [](json& j) { j["orders"]["seed"] = "42"; }, "orders.seed");
    ok &= rejects("booleano equivocado", [](json& j) { j["streets"] = json::parse(R"([{"id":"s0","from":"n0","to":"n0","oneWay":"yes"}])"); }, "streets[0].oneWay");
    ok &= rejects("lista que no es lista", [](json& j) { j["nodes"] = json::object(); }, "nodes: debe ser una lista");
    ok &= rejects("prepTimeMs con un solo valor", [](json& j) { j["restaurants"][0]["prepTimeMs"] = {1000}; }, "restaurants[0].prepTimeMs");
    ok &= rejects("sección que no es objeto", [](json& j) { j["simulation"] = 5; }, "simulation: debe ser un objeto");

    // Referencias a nodos (las pide el enunciado explícitamente) e ids repetidos.
    ok &= rejects("calle hacia un nodo inexistente", [](json& j) { j["streets"] = json::parse(R"([{"id":"s0","from":"n0","to":"n9","oneWay":false}])"); }, "streets[0].to: el nodo 'n9' no existe");
    ok &= rejects("restaurante en un nodo inexistente", [](json& j) { j["restaurants"][0]["node"] = "n9"; }, "restaurants[0].node: el nodo 'n9' no existe");
    ok &= rejects("startNode inexistente", [](json& j) { j["fleet"]["startNode"] = "n9"; }, "fleet.startNode: el nodo 'n9' no existe");
    ok &= rejects("id de nodo repetido", [](json& j) { j["nodes"].push_back(j["nodes"][0]); }, "nodes[1].id: el id 'n0' está repetido");
    ok &= rejects("id de restaurante repetido", [](json& j) { j["restaurants"].push_back(j["restaurants"][0]); }, "restaurants[1].id");
    ok &= rejects("sin nodos", [](json& j) { j["nodes"] = json::array(); }, "nodes: debe haber al menos un nodo");
    ok &= rejects("sin restaurantes", [](json& j) { j["restaurants"] = json::array(); }, "restaurants: debe haber al menos un restaurante");

    // Rangos de la tabla del enunciado y los necesarios para que la simulación tenga sentido.
    ok &= rejects("sin motos", [](json& j) { j["fleet"]["couriers"] = 0; }, "fleet.couriers: debe ser al menos 1 (es 0)");
    ok &= rejects("bolsa vacía", [](json& j) { j["fleet"]["bagCapacity"] = 0; }, "fleet.bagCapacity");
    ok &= rejects("sin puestos de recogida", [](json& j) { j["restaurants"][0]["pickupSlots"] = 0; }, "restaurants[0].pickupSlots");
    ok &= rejects("prepTimeMs con mínimo > máximo", [](json& j) { j["restaurants"][0]["prepTimeMs"] = {2000, 1000}; }, "restaurants[0].prepTimeMs: debe ser [mínimo, máximo]");
    ok &= rejects("ráfaga de 0 pedidos", [](json& j) { j["orders"]["burstMax"] = 0; }, "orders.burstMax");
    ok &= rejects("intervalo entre pedidos 0", [](json& j) { j["orders"]["meanIntervalMs"] = 0; }, "orders.meanIntervalMs");
    ok &= rejects("maxPending negativo", [](json& j) { j["orders"]["maxPending"] = -1; }, "orders.maxPending");
    ok &= rejects("timeout negativo", [](json& j) { j["dispatch"]["quoteTimeoutMs"] = -5; }, "dispatch.quoteTimeoutMs");
    ok &= rejects("probabilidad de avería > 1", [](json& j) { j["incidents"]["breakdownProbability"] = 1.5; }, "incidents.breakdownProbability: debe estar entre 0 y 1");
    ok &= rejects("probabilidad de avería < 0", [](json& j) { j["incidents"]["breakdownProbability"] = -0.1; }, "incidents.breakdownProbability");
    ok &= rejects("velocidad 0", [](json& j) { j["fleet"]["speedKmh"] = 0; }, "fleet.speedKmh: debe ser mayor que 0");
    ok &= rejects("duración negativa", [](json& j) { j["simulation"]["durationS"] = -1; }, "simulation.durationS");
    ok &= rejects("timeScale 0", [](json& j) { j["simulation"]["timeScale"] = 0; }, "simulation.timeScale: debe ser mayor que 0");
    ok &= rejects("bounds invertidos", [](json& j) { j["map"]["bounds"]["north"] = -1.0; }, "map.bounds: debe cumplir north > south");

    // Los bordes válidos se aceptan: probabilidad 0 y 1, maxPending 0, durationS 0 (hasta una señal).
    json edges = minimal();
    edges["incidents"]["breakdownProbability"] = 1.0;
    edges["orders"]["maxPending"] = 0;
    edges["simulation"]["durationS"] = 0;
    ok &= check("bordes válidos: probabilidad 1, maxPending 0, durationS 0",
                parseConfig(edges.dump(), "config").breakdownProbability == 1.0);

    // 3. Ruta de la imagen: relativa a la carpeta de la configuración; absoluta, tal cual.
    json absolute = minimal();
    absolute["map"]["image"] = "/tmp/x.png";
    ok &= check("imagen relativa a la carpeta del archivo",
                parseConfig(minimal().dump(), "config").mapImage == "config/../data/equipetrol.png");
    ok &= check("imagen con ruta absoluta", parseConfig(absolute.dump(), "config").mapImage == "/tmp/x.png");
    return ok ? 0 : 1;
}
