#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>

// Cuánto dura entregar la comida en el puesto: el enunciado dice que consume tiempo simulado y
// la configuración no trae el valor (decisión del proyecto).
constexpr std::int64_t kHandoverMs = 30000;

// Un restaurante con `pickupSlots` puestos de recogida: un monitor (como 07/example_42).
//
// "En ningún instante puede haber más repartidores recogiendo comida allí que puestos
// disponibles. Los demás hacen fila y son atendidos en orden de llegada."
//
// Un condition_variable solo NO da orden de llegada: notify despierta a cualquiera, y un hilo
// recién llegado puede ganarle el lock a otro que ya esperaba. Por eso cada moto que llega
// toma un TICKET (número correlativo, con el lock tomado) y solo entra el ticket siguiente
// en ser atendido, y solo si hay un puesto libre: como la fila con números de un banco.
//
// _inUse, _nextTicket, _nowServing y _aborted son privados y solo se tocan con _mutex
// tomado: no hay data race posible sobre ellos.
class Restaurant {
public:
    // Un puesto ocupado. Lo libera al destruirse (como lock_guard con un mutex): si la moto
    // abandona a mitad de la recogida (p. ej. por la detención), el puesto no queda tomado.
    // Move-only: un puesto lo tiene una sola moto a la vez.
    class Slot {
    public:
        Slot(Slot&& other) noexcept : _restaurant(other._restaurant), _ticket(other._ticket)
        {
            other._restaurant = nullptr;
        }
        Slot& operator=(Slot&&) = delete;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        ~Slot()
        {
            if (_restaurant != nullptr) {
                _restaurant->leave();
            }
        }

        // Número de llegada: 0 para la primera moto que llegó, 1 para la segunda...
        std::uint64_t ticket() const { return _ticket; }

    private:
        friend class Restaurant;
        Slot(Restaurant& restaurant, std::uint64_t ticket) : _restaurant(&restaurant), _ticket(ticket) {}

        Restaurant* _restaurant;  // no es dueño: el restaurante vive más que cualquier puesto
        std::uint64_t _ticket;
    };

    Restaurant(std::string id, std::size_t pickupSlots);
    Restaurant(const Restaurant&) = delete;
    Restaurant& operator=(const Restaurant&) = delete;

    const std::string& id() const { return _id; }

    // Llega una moto: hace fila y bloquea SIN consumir CPU hasta que sea su turno y haya un
    // puesto libre. Devuelve std::nullopt si se abortó (detención) antes o durante la espera.
    std::optional<Slot> enter();

    // Detención: despierta a todos los que hacen fila (enter devuelve std::nullopt) y hace que
    // los que lleguen después tampoco esperen. Los puestos ya tomados se liberan solos.
    void abort();

    std::size_t inUse() const;

private:
    void leave();

    const std::string _id;
    const std::size_t _slots;

    mutable std::mutex _mutex;
    std::condition_variable _turn;
    std::size_t _inUse = 0;
    std::uint64_t _nextTicket = 0;  // el que tomará la próxima moto que llegue
    std::uint64_t _nowServing = 0;  // el próximo ticket que puede entrar
    bool _aborted = false;
};

// Los restaurantes de la simulación por id. El mapa se arma antes de lanzar hilos y no cambia;
// cada Restaurant es un monitor que comparten las motos que llegan.
using Restaurants = std::unordered_map<std::string, std::shared_ptr<Restaurant>>;
