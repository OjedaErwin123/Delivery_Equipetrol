#include "config.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

// Cada función recibe el objeto que contiene la propiedad, su nombre y la ruta completa
// (p. ej. "fleet.couriers") para que el mensaje de error diga exactamente qué falla.
// property() y array() devuelven un PUNTERO al valor dentro del JSON (nunca nullptr: si
// falta, lanzan). Con una referencia, g++ avisa -Wdangling-reference cada vez que la ruta es
// un temporal, porque no puede descartar que la referencia devuelta apunte a él.

const json* property(const json& object, const char* key, const std::string& path)
{
    if (!object.is_object()) {
        throw ConfigError(path.substr(0, path.rfind('.')) + ": debe ser un objeto");
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        throw ConfigError("falta la propiedad obligatoria " + path);
    }
    return &*it;
}

// Lee la propiedad, comprueba su tipo con `isType` (un método de json, p. ej. is_string) y la convierte.
template <class T>
T value(const json& object, const char* key, const std::string& path, bool (json::*isType)() const noexcept,
        const char* expected)
{
    const json& v = *property(object, key, path);
    if (!(v.*isType)()) {
        throw ConfigError(path + ": debe ser " + expected);
    }
    return v.get<T>();
}

std::string text(const json& o, const char* k, const std::string& p)
{
    return value<std::string>(o, k, p, &json::is_string, "un texto");
}
std::int64_t integer(const json& o, const char* k, const std::string& p)  // 3.5 o "3" no valen
{
    return value<std::int64_t>(o, k, p, &json::is_number_integer, "un número entero");
}
double number(const json& o, const char* k, const std::string& p)
{
    return value<double>(o, k, p, &json::is_number, "un número");
}
bool boolean(const json& o, const char* k, const std::string& p)
{
    return value<bool>(o, k, p, &json::is_boolean, "true o false");
}

const json* array(const json& object, const char* key, const std::string& path)
{
    const json* v = property(object, key, path);
    if (!v->is_array()) {
        throw ConfigError(path + ": debe ser una lista");
    }
    return v;
}

// "../data/x.png" relativo a "config/" → "config/../data/x.png". Sin std::filesystem,
// que no existe en g++ 7 (el mínimo del enunciado).
std::string resolve(const std::string& baseDir, const std::string& file)
{
    if (file.empty() || file[0] == '/' || baseDir.empty()) {
        return file;
    }
    return baseDir + "/" + file;
}

// --- Validaciones de significado: las propiedades ya existen y tienen el tipo correcto ---

void require(bool ok, const std::string& message)
{
    if (!ok) {
        throw ConfigError(message);
    }
}

// Comprueba que los ids de una lista no se repitan y devuelve el conjunto de ids.
template <class Item>
std::set<std::string> uniqueIds(const std::vector<Item>& items, const std::string& list)
{
    std::set<std::string> ids;
    for (std::size_t i = 0; i < items.size(); ++i) {
        require(ids.insert(items[i].id).second,
                list + "[" + std::to_string(i) + "].id: el id '" + items[i].id + "' está repetido");
    }
    return ids;
}

std::string atLeast(const std::string& path, std::int64_t min, std::int64_t value)
{
    return path + ": debe ser al menos " + std::to_string(min) + " (es " + std::to_string(value) + ")";
}

void validate(const Config& c)
{
    require(c.north > c.south && c.east > c.west, "map.bounds: debe cumplir north > south y east > west");
    require(!c.nodes.empty(), "nodes: debe haber al menos un nodo");
    const std::set<std::string> nodes = uniqueIds(c.nodes, "nodes");
    uniqueIds(c.streets, "streets");
    uniqueIds(c.restaurants, "restaurants");
    auto nodeExists = [&nodes](const std::string& id, const std::string& path) {
        require(nodes.count(id) > 0, path + ": el nodo '" + id + "' no existe");
    };

    for (std::size_t i = 0; i < c.streets.size(); ++i) {
        const std::string p = "streets[" + std::to_string(i) + "]";
        nodeExists(c.streets[i].from, p + ".from");
        nodeExists(c.streets[i].to, p + ".to");
    }
    require(!c.restaurants.empty(), "restaurants: debe haber al menos un restaurante");
    for (std::size_t i = 0; i < c.restaurants.size(); ++i) {
        const RestaurantConfig& r = c.restaurants[i];
        const std::string p = "restaurants[" + std::to_string(i) + "]";
        nodeExists(r.node, p + ".node");
        require(r.pickupSlots >= 1, atLeast(p + ".pickupSlots", 1, r.pickupSlots));
        require(r.prepMinMs >= 0 && r.prepMinMs <= r.prepMaxMs,
                p + ".prepTimeMs: debe ser [mínimo, máximo] con 0 <= mínimo <= máximo");
    }

    nodeExists(c.startNode, "fleet.startNode");
    require(c.couriers >= 1, atLeast("fleet.couriers", 1, c.couriers));
    require(c.bagCapacity >= 1, atLeast("fleet.bagCapacity", 1, c.bagCapacity));
    require(c.speedKmh > 0, "fleet.speedKmh: debe ser mayor que 0");
    require(c.meanIntervalMs >= 1, atLeast("orders.meanIntervalMs", 1, c.meanIntervalMs));
    require(c.burstMax >= 1, atLeast("orders.burstMax", 1, c.burstMax));
    require(c.maxPending >= 0, atLeast("orders.maxPending", 0, c.maxPending));
    require(c.quoteTimeoutMs >= 0, atLeast("dispatch.quoteTimeoutMs", 0, c.quoteTimeoutMs));
    require(c.acceptTimeoutMs >= 0, atLeast("dispatch.acceptTimeoutMs", 0, c.acceptTimeoutMs));
    require(c.breakdownProbability >= 0 && c.breakdownProbability <= 1,
            "incidents.breakdownProbability: debe estar entre 0 y 1");
    require(c.durationS >= 0, atLeast("simulation.durationS", 0, c.durationS));
    require(c.timeScale > 0, "simulation.timeScale: debe ser mayor que 0");
}

}  // namespace

Config parseConfig(const std::string& textIn, const std::string& baseDir)
{
    json j;
    try {
        j = json::parse(textIn);
    } catch (const json::parse_error& e) {
        throw ConfigError(std::string("el JSON está mal formado: ") + e.what());
    }
    if (!j.is_object()) {
        throw ConfigError("la configuración debe ser un objeto JSON");
    }

    Config c;
    const json& map = *property(j, "map", "map");
    c.mapImage = resolve(baseDir, text(map, "image", "map.image"));
    c.attribution = text(map, "attribution", "map.attribution");
    const json& bounds = *property(map, "bounds", "map.bounds");
    c.north = number(bounds, "north", "map.bounds.north");
    c.south = number(bounds, "south", "map.bounds.south");
    c.west = number(bounds, "west", "map.bounds.west");
    c.east = number(bounds, "east", "map.bounds.east");

    const json& nodes = *array(j, "nodes", "nodes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::string p = "nodes[" + std::to_string(i) + "]";
        c.nodes.push_back({text(nodes[i], "id", p + ".id"), number(nodes[i], "lat", p + ".lat"),
                           number(nodes[i], "lon", p + ".lon")});
    }
    const json& streets = *array(j, "streets", "streets");
    for (std::size_t i = 0; i < streets.size(); ++i) {
        const std::string p = "streets[" + std::to_string(i) + "]";
        c.streets.push_back({text(streets[i], "id", p + ".id"), text(streets[i], "from", p + ".from"),
                             text(streets[i], "to", p + ".to"), boolean(streets[i], "oneWay", p + ".oneWay")});
    }
    const json& restaurants = *array(j, "restaurants", "restaurants");
    for (std::size_t i = 0; i < restaurants.size(); ++i) {
        const std::string p = "restaurants[" + std::to_string(i) + "]";
        const json& r = restaurants[i];
        const json& prep = *array(r, "prepTimeMs", p + ".prepTimeMs");
        if (prep.size() != 2 || !prep[0].is_number_integer() || !prep[1].is_number_integer()) {
            throw ConfigError(p + ".prepTimeMs: debe ser una lista [mínimo, máximo] de dos enteros");
        }
        c.restaurants.push_back({text(r, "id", p + ".id"), text(r, "name", p + ".name"),
                                 text(r, "node", p + ".node"), integer(r, "pickupSlots", p + ".pickupSlots"),
                                 prep[0].get<std::int64_t>(), prep[1].get<std::int64_t>()});
    }

    const json& fleet = *property(j, "fleet", "fleet");
    c.couriers = integer(fleet, "couriers", "fleet.couriers");
    c.bagCapacity = integer(fleet, "bagCapacity", "fleet.bagCapacity");
    c.speedKmh = number(fleet, "speedKmh", "fleet.speedKmh");
    c.startNode = text(fleet, "startNode", "fleet.startNode");

    const json& orders = *property(j, "orders", "orders");
    c.meanIntervalMs = integer(orders, "meanIntervalMs", "orders.meanIntervalMs");
    c.burstMax = integer(orders, "burstMax", "orders.burstMax");
    c.maxPending = integer(orders, "maxPending", "orders.maxPending");
    c.seed = integer(orders, "seed", "orders.seed");

    const json& dispatch = *property(j, "dispatch", "dispatch");
    c.quoteTimeoutMs = integer(dispatch, "quoteTimeoutMs", "dispatch.quoteTimeoutMs");
    c.acceptTimeoutMs = integer(dispatch, "acceptTimeoutMs", "dispatch.acceptTimeoutMs");

    const json& incidents = *property(j, "incidents", "incidents");
    c.breakdownProbability = number(incidents, "breakdownProbability", "incidents.breakdownProbability");

    const json& simulation = *property(j, "simulation", "simulation");
    c.durationS = integer(simulation, "durationS", "simulation.durationS");
    c.timeScale = number(simulation, "timeScale", "simulation.timeScale");
    validate(c);
    return c;
}

Config loadConfig(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        throw ConfigError("no se pudo abrir el archivo de configuración '" + path + "'");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto slash = path.rfind('/');
    return parseConfig(buffer.str(), slash == std::string::npos ? "" : path.substr(0, slash));
}
