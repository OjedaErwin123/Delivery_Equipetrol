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

    // 2. Un caso de cada tipo de error se rechaza con un mensaje que nombra el problema
    //    (la tabla completa de casos inválidos llega con las validaciones de significado).
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

    // 3. Ruta de la imagen: relativa a la carpeta de la configuración; absoluta, tal cual.
    json absolute = minimal();
    absolute["map"]["image"] = "/tmp/x.png";
    ok &= check("imagen relativa a la carpeta del archivo",
                parseConfig(minimal().dump(), "config").mapImage == "config/../data/equipetrol.png");
    ok &= check("imagen con ruta absoluta", parseConfig(absolute.dump(), "config").mapImage == "/tmp/x.png");
    return ok ? 0 : 1;
}
