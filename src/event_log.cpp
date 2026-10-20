#include "event_log.hpp"

#include <algorithm>
#include <cstdio>

namespace {

// Escribe `value` como string JSON entre comillas, escapando lo que JSON exige.
void appendJsonString(std::string& out, const std::string& value)
{
    out += '"';
    for (const char ch : value) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {  // otros caracteres de control
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(ch));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

void appendField(std::string& out, const char* key, const std::string& value)
{
    if (value.empty()) {
        return;
    }
    out += ", \"";
    out += key;
    out += "\": ";
    appendJsonString(out, value);
}

}  // namespace

std::string toJsonLine(const Event& e)
{
    // Mismo formato que los ejemplos del enunciado: {"t": 15230, "event": "orderCreated", ...}
    std::string line = "{\"t\": " + std::to_string(e.t) + ", \"event\": ";
    appendJsonString(line, e.event);
    appendField(line, "order", e.order);
    appendField(line, "courier", e.courier);
    appendField(line, "restaurant", e.restaurant);
    appendField(line, "from", e.from);
    appendField(line, "to", e.to);
    appendField(line, "reason", e.reason);
    if (e.arrivedAt) {
        // Propiedad extra (no está en la tabla 6.2): sin ella, el log no permite verificar la
        // atención por orden de llegada en los restaurantes.
        line += ", \"arrivedAt\": " + std::to_string(*e.arrivedAt);
    }
    line += '}';
    return line;
}

void EventLog::write(const Event& e)
{
    const std::string line = toJsonLine(e) + '\n';  // fuera del lock
    std::lock_guard<std::mutex> lock(_mutex);
    _out << line;
    count(e);  // en la misma sección crítica: los contadores van a la par del log
}

LiveStats EventLog::stats() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _stats;
}

void EventLog::count(const Event& e)
{
    if (e.event == "orderCreated") {
        ++_stats.created;
        _createdAt[e.order] = e.t;
        _stats.activeDestinations[e.order] = e.destination;
    } else if (e.event == "orderRejected") {
        ++_stats.rejected;
        _createdAt.erase(e.order);
        _stats.activeDestinations.erase(e.order);
    } else if (e.event == "orderDelivered") {
        const std::int64_t took = e.t - _createdAt[e.order];
        ++_stats.delivered;
        _stats.totalDeliveryMs += took;
        _stats.maxDeliveryMs = std::max(_stats.maxDeliveryMs, took);
        ++_stats.deliveredPerCourier[e.courier];
        _createdAt.erase(e.order);
        _stats.activeDestinations.erase(e.order);
    } else if (e.event == "orderReassigned") {
        ++_stats.reassigned;
    } else if (e.event == "courierBreakdown") {
        ++_stats.breakdowns;
    }
}
