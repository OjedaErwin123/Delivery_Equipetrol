#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
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
// (check-then-act). _assigned, _broken y _aborted solo se tocan con _mutex tomado: no hay data race.
//
// Una moto averiada no tiene lugar para nadie: no es candidata ni se le puede reservar cupo.
// Prioridad de los traspasos: mientras una moto averiada busca receptor (beginTransfer ..
// endTransfer), el despachador no reserva en ninguna moto, así el traspaso no pasa hambre.
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
    bool waitForChange(std::uint64_t seen);  // sin plazo (la moto averiada que espera receptor)

    // Traspasos: una moto averiada empieza a buscar receptor / ya traspasó (o se detuvo).
    void beginTransfer();
    void endTransfer();
    // Las motos (distintas de `except`) que podrían recibir `count` pedidos: no averiadas, vivas y con
    // lugar para todos. Una foto, como withSpace: se revalida al traspasar.
    std::vector<std::size_t> canReceive(std::size_t count, std::size_t except) const;
    // La moto terminó su hilo: ya no recibe nada. ¿Queda alguna otra viva?
    void retire(std::size_t courier);
    bool othersAlive(std::size_t except) const;

    // La moto `courier` terminó un pedido (lo entregó o no pudo tomarlo): libera su cupo.
    void release(std::size_t courier);

    // Traspaso: pasar los `count` cupos de `from` a `to` si `to` no está averiada y le alcanza el
    // lugar. Con el lock tomado y la validación hecha llama a `commit` (que mueve los pedidos y los
    // registra): si devuelve false (detención), la flota queda como estaba. Todo o nada.
    bool moveAssigned(std::size_t from, std::size_t to, std::size_t count, const std::function<bool()>& commit);

    // La moto se averió (true) o ya se reparó (false). Al repararse cambia la versión: el despachador
    // que esperaba una moto con lugar se despierta.
    void setBroken(std::size_t courier, bool broken);

    // Detención: despierta al que espera en reserve (devuelve std::nullopt).
    void abort();

    std::size_t assigned(std::size_t courier) const;

private:
    bool hasSpace(std::size_t courier) const
    {
        return _transfers == 0 && !_broken[courier] && !_retired[courier] && _assigned[courier] < _capacity;
    }

    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable _space;
    std::vector<std::size_t> _assigned;
    std::vector<bool> _broken;
    std::vector<bool> _retired;
    std::size_t _transfers = 0;  // motos averiadas buscando receptor
    bool _aborted = false;
    std::uint64_t _version = 0;
};
