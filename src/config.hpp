#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// La configuración de la simulación (sección 5 del enunciado), ya convertida a tipos
// de C++. El resto del programa nunca toca el JSON: recibe un Config ya leído.
//
// Se lee una sola vez, en el hilo principal y antes de lanzar ningún hilo; después
// los hilos solo la leen, así que no puede haber data races sobre ella.

struct NodeConfig {
    std::string id;
    double lat = 0, lon = 0;
};

struct StreetConfig {
    std::string id, from, to;
    bool oneWay = false;
};

struct RestaurantConfig {
    std::string id, name, node;
    std::int64_t pickupSlots = 0;
    std::int64_t prepMinMs = 0, prepMaxMs = 0;
};

struct Config {
    // map
    std::string mapImage;  // ruta resuelta: relativa a la carpeta del archivo de configuración
    std::string attribution;
    double north = 0, south = 0, west = 0, east = 0;

    std::vector<NodeConfig> nodes;
    std::vector<StreetConfig> streets;
    std::vector<RestaurantConfig> restaurants;

    // fleet
    std::int64_t couriers = 0, bagCapacity = 0;
    double speedKmh = 0;
    std::string startNode;

    // orders
    std::int64_t meanIntervalMs = 0, burstMax = 0, maxPending = 0, seed = 0;

    // dispatch: quoteTimeoutMs es tiempo REAL; acceptTimeoutMs, tiempo SIMULADO
    std::int64_t quoteTimeoutMs = 0, acceptTimeoutMs = 0;

    // incidents
    double breakdownProbability = 0;

    // simulation: durationS = 0 significa "hasta recibir una señal"
    std::int64_t durationS = 0;
    double timeScale = 0;
};

// Una configuración que no se puede usar. El mensaje es legible y dice qué propiedad falla.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Lee, convierte y valida el archivo. Lanza ConfigError si no se puede leer, si el JSON está
// mal formado, si falta una propiedad obligatoria o tiene el tipo equivocado, si una calle,
// un restaurante o el startNode apuntan a un nodo que no existe, si hay ids repetidos o si un
// valor está fuera de rango (p. ej. couriers < 1 o timeScale <= 0).
Config loadConfig(const std::string& path);

// Lo mismo a partir del texto; `baseDir` es la carpeta contra la que se resuelve map.image.
Config parseConfig(const std::string& text, const std::string& baseDir);
