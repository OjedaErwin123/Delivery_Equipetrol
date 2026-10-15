// Prueba con nlohmann/json de que el JSON que el programa escribe a mano es JSON válido,
// como lo comprobará el verificador del revisor: los eventos (ida y vuelta, también con
// caracteres especiales), el resumen y todas las líneas de una simulación real.
// Sale con 0 si todo está bien.
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "event_log.hpp"
#include "log_line.hpp"
#include "simulation.hpp"

using nlohmann::json;

namespace {

// Escribe el evento con toJsonLine, lo lee con nlohmann/json y compara campo por campo.
bool roundTrip(const Event& e)
{
    const json j = json::parse(toJsonLine(e));
    bool ok = j.at("t").is_number_integer() && j.at("t").get<std::int64_t>() == e.t &&
              j.at("event").get<std::string>() == e.event;
    const std::vector<std::pair<const char*, const std::string*>> fields = {
        {"order", &e.order}, {"courier", &e.courier}, {"restaurant", &e.restaurant},
        {"from", &e.from},   {"to", &e.to},           {"reason", &e.reason}};
    for (const auto& [key, value] : fields) {
        ok = ok && (value->empty() ? !j.contains(key) : j.at(key).get<std::string>() == *value);
    }
    return ok;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Ida y vuelta de un evento de cada forma que usa el enunciado.
    std::vector<Event> events(5);
    events[0].t = 15230, events[0].event = "orderCreated", events[0].order = "o17", events[0].restaurant = "r3";
    events[1].t = 15410, events[1].event = "orderAssigned", events[1].order = "o17", events[1].courier = "c5";
    events[2].t = 322900, events[2].event = "orderRejected", events[2].order = "o21", events[2].reason = "unreachable";
    events[3].t = 400000, events[3].event = "orderReassigned", events[3].order = "o9", events[3].from = "c1",
    events[3].to = "c2";
    events[4].t = 1, events[4].event = "orderPickedUp", events[4].order = "o\"x\\y\n\t\x01ñ", events[4].courier = "c0",
    events[4].restaurant = "r0";  // comillas, barras, saltos, control y UTF-8
    bool allEvents = true;
    for (const Event& e : events) {
        allEvents = allEvents && roundTrip(e);
    }
    ok &= check("eventos: JSON válido y mismos valores al leerlos", allEvents);

    // 2. Una simulación real chica: cada línea del log es un objeto JSON con t entero y event,
    //    y el resumen es JSON válido, coherente con el log.
    SimParams p;
    p.couriers = 3;
    p.orders = 20;
    p.meanIntervalMs = 1000;
    p.legMs = 2000;
    p.timeScale = 10000.0;
    std::ostringstream logOut;
    std::ostringstream out;
    const Summary summary = runSimulation(p, logOut, out);

    bool allLines = true;
    int created = 0;
    int delivered = 0;
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const json j = json::parse(line);
        allLines = allLines && j.is_object() && j.at("t").is_number_integer() && j.at("event").is_string();
        created += j.at("event") == "orderCreated";
        delivered += j.at("event") == "orderDelivered";
    }
    ok &= check("log: cada línea es un objeto JSON con t entero y event", allLines);

    const json s = json::parse(toJson(summary));
    int perCourier = 0;
    for (const auto& item : s.at("deliveredPerCourier").items()) {
        perCourier += item.value().get<int>();
    }
    ok &= check("resumen: JSON válido con todos los campos del enunciado",
                s.at("created").is_number_integer() && s.at("delivered").is_number_integer() &&
                    s.at("rejected").is_number_integer() && s.at("pending").is_number_integer() &&
                    s.at("reassigned").is_number_integer() && s.at("breakdowns").is_number_integer() &&
                    s.at("avgDeliveryMs").is_number_integer() && s.at("maxDeliveryMs").is_number_integer() &&
                    s.at("deliveredPerCourier").is_object());
    ok &= check("resumen: created = delivered + rejected + pending",
                s.at("created") == s.at("delivered").get<int>() + s.at("rejected").get<int>() +
                                       s.at("pending").get<int>());
    ok &= check("resumen: contadores iguales al log y entregas por moto suman delivered",
                s.at("created") == created && s.at("delivered") == delivered && perCourier == delivered);
    return ok ? 0 : 1;
}
