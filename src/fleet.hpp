#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

// El cupo de las mochilas de la flota: cuántos pedidos tiene asignados (y sin entregar) cada
// moto, con un máximo de `bagCapacity` por moto. Un monitor (como 07/example_42).
//
// Los contadores los cambian hilos distintos: el despachador al asignar (sube) y cada moto al
// terminar un pedido (baja). "Elegir una moto con lugar" y "reservarle el cupo" son UNA sola
// operación con el lock tomado (reserve): si el despachador mirara primero si hay lugar y
// después asignara, entre las dos cosas otra asignación podría llenar la mochila
// (check-then-act). _assigned y _aborted solo se tocan con _mutex tomado: no hay data race.
class Fleet {
public:
    Fleet(std::size_t couriers, std::size_t bagCapacity);
    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    // Busca, por turnos a partir de `from`, una moto con lugar, le reserva un cupo y devuelve su
    // índice. Si todas están llenas, duerme SIN consumir CPU hasta que alguna libere uno.
    // std::nullopt si se abortó (detención).
    std::optional<std::size_t> reserve(std::size_t from);

    // Las motos con lugar en la mochila: los candidatos del despachador. Es una foto: puede dejar de
    // valer enseguida, por eso después se reserva con tryReserve (revalidar).
    std::vector<std::size_t> withSpace() const;

    // Reservar un cupo en ESA moto si todavía tiene lugar (comprobar y reservar con el mismo lock).
    // false si se llenó desde que se estimó: el despachador prueba con la siguiente mejor.
    bool tryReserve(std::size_t courier);

    // Cada release cambia la versión. El despachador la lee ANTES de estimar y, si la ronda no le
    // sirvió, espera a que cambie (alguna moto liberó un cupo) o a `deadline`: así no se pierde un
    // aviso que llegó mientras estimaba, y no gira reintentando. false si se abortó.
    std::uint64_t version() const;
    bool waitForChange(std::uint64_t seen, std::chrono::steady_clock::time_point deadline);

    // La moto `courier` terminó un pedido (lo entregó o no pudo tomarlo): libera su cupo.
    void release(std::size_t courier);

    // Detención: despierta al que espera en reserve (devuelve std::nullopt).
    void abort();

    std::size_t assigned(std::size_t courier) const;

private:
    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable _space;
    std::vector<std::size_t> _assigned;
    bool _aborted = false;
    std::uint64_t _version = 0;
};
