// Prueba del traspaso de mochila entre dos motos: todo o nada (los pedidos pasan juntos, al
// principio de la mochila del receptor, con un orderReassigned cada uno y los cupos de la flota);
// se rechaza sin mover nada si el receptor está lleno, averiado o cerrado; y dos hilos que
// traspasan A→B y B→A a la vez, una y otra vez, no se bloquean ni pierden ni duplican pedidos.
// Sale con 0 si todo está bien.
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"

namespace {

int countLines(const std::string& text, const std::string& event)
{
    int n = 0;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        n += field(line, "event") == event;
    }
    return n;
}

std::vector<std::string> destinations(const Courier& courier)
{
    std::vector<std::string> out;
    for (const PlannedOrder& o : courier.plan().orders) {
        out.push_back(o.destination);
    }
    return out;
}

// Reserva el cupo en la flota y asigna, como hace el despachador.
bool give(Courier& courier, Fleet& fleet, std::size_t index, const std::string& destination)
{
    return fleet.tryReserve(index) &&
           courier.assign(Order("o-" + destination, "r0", destination, 0)) == OrderBook::Submit::accepted;
}

}  // namespace

int main()
{
    bool ok = true;
    const SimClock clock(1000.0);
    const Shutdown shutdown;

    // 1. A (con x1, x2) le traspasa todo a B (con y1): B queda x1, x2, y1 y A vacía.
    {
        std::ostringstream out;
        EventLog log(out);
        auto fleet = std::make_shared<Fleet>(2, 3);
        Courier a("c0", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 0);
        Courier b("c1", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 1);
        ok &= check("asignados", give(a, *fleet, 0, "x1") && give(a, *fleet, 0, "x2") && give(b, *fleet, 1, "y1"));
        const bool done = a.transferTo(b) == Courier::Handover::done;
        std::cout << "B después del traspaso:";
        for (const std::string& d : destinations(b)) {
            std::cout << " " << d;
        }
        std::cout << "\n" << out.str();
        ok &= check("todo pasa a B, al principio y en su orden; A queda vacía",
                    done && destinations(b) == std::vector<std::string>{"x1", "x2", "y1"} && destinations(a).empty());
        ok &= check("los cupos pasan con los pedidos", fleet->assigned(0) == 0 && fleet->assigned(1) == 3);
        ok &= check("un orderReassigned por pedido, de c0 a c1", countLines(out.str(), "orderReassigned") == 2 &&
                                                                    out.str().find("\"from\": \"c0\", \"to\": \"c1\"") !=
                                                                        std::string::npos &&
                                                                    a.reassigned() == 2);
    }

    // 2. Rechazos: B llena, C averiada, D detenida (abort). No se mueve nada ni se registra nada.
    //    (Cerrada sí recibe: solo significa que el despachador no le asigna más.)
    {
        std::ostringstream out;
        EventLog log(out);
        auto fleet = std::make_shared<Fleet>(4, 2);
        Courier a("c0", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 0);
        Courier b("c1", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 1);
        Courier c("c2", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 2);
        Courier d("c3", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 3);
        give(a, *fleet, 0, "x1");
        give(b, *fleet, 1, "y1");
        give(b, *fleet, 1, "y2");
        fleet->setBroken(2, true);
        d.abort();
        const bool refused = a.transferTo(b) == Courier::Handover::refused &&
                             a.transferTo(c) == Courier::Handover::refused &&
                             a.transferTo(d) == Courier::Handover::refused && a.transferTo(a) == Courier::Handover::refused;
        ok &= check("receptor lleno, averiado o detenido: rechazado, nada se mueve ni se registra",
                    refused && destinations(a) == std::vector<std::string>{"x1"} && destinations(b).size() == 2 &&
                        fleet->assigned(0) == 1 && out.str().empty());
    }

    // 3. Dos hilos traspasan A→B y B→A a la vez, 2000 veces cada uno, con 20 pedidos por moto. Con
    //    scoped_lock no hay deadlock (si lo hubiera, la prueba se colgaría) y, al final, los 40
    //    pedidos siguen estando cada uno en una sola mochila. TSan vigila los accesos.
    {
        std::ostringstream out;
        EventLog log(out);
        auto fleet = std::make_shared<Fleet>(2, 100);
        Courier a("c0", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 0);
        Courier b("c1", clock, log, shutdown, 1000, nullptr, nullptr, fleet, 1);
        for (int i = 0; i < 20; ++i) {
            give(a, *fleet, 0, "a" + std::to_string(i));
            give(b, *fleet, 1, "b" + std::to_string(i));
        }
        // Los dos arrancan juntos (esperan el mismo shared_future, sin girar): así compiten de verdad.
        std::promise<void> go;
        const std::shared_future<void> start = go.get_future().share();
        std::thread ab([&] {
            start.wait();
            for (int i = 0; i < 2000; ++i) {
                a.transferTo(b);
            }
        });
        std::thread ba([&] {
            start.wait();
            for (int i = 0; i < 2000; ++i) {
                b.transferTo(a);
            }
        });
        go.set_value();
        ab.join();
        ba.join();
        std::set<std::string> all;
        for (const std::string& d : destinations(a)) {
            all.insert(d);
        }
        for (const std::string& d : destinations(b)) {
            all.insert(d);
        }
        const std::size_t inA = destinations(a).size(), inB = destinations(b).size();
        std::cout << "traspasos cruzados: " << countLines(out.str(), "orderReassigned")
                  << " pedidos movidos; al final A tiene " << inA << " y B " << inB << "\n";
        ok &= check("sin deadlock; ningún pedido perdido ni duplicado (40 distintos)", inA + inB == 40 && all.size() == 40);
        ok &= check("la flota coincide con las mochilas", fleet->assigned(0) == inA && fleet->assigned(1) == inB);
        ok &= check("cada pedido movido tiene su línea en el log",
                    countLines(out.str(), "orderReassigned") == a.reassigned() + b.reassigned());
    }
    return ok ? 0 : 1;
}
