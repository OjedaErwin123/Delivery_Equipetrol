// Prueba del registro de eventos: varios hilos escriben a la vez y se verifica que
// cada línea sea un evento completo (nunca dos mezclados), que estén todos una sola
// vez y que los de cada hilo conserven su orden. Sale con 0 si todo está bien.
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "event_log.hpp"

namespace {

constexpr int kWriters = 8;
constexpr int kEventsPerWriter = 2000;

// El evento i del hilo w: su contenido identifica de forma única quién lo escribió.
Event makeEvent(int w, int i)
{
    Event e;
    e.t = i;
    e.event = "orderAssigned";
    e.order = "o" + std::to_string(w * kEventsPerWriter + i);
    e.courier = "c" + std::to_string(w);
    return e;
}

bool check(const char* name, bool ok)
{
    std::cout << name << ": " << (ok ? "OK" : "FALLA") << "\n";
    return ok;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Formato: igual al de los ejemplos del enunciado, solo con los campos presentes.
    Event created;
    created.t = 15230;
    created.event = "orderCreated";
    created.order = "o17";
    created.restaurant = "r3";
    ok &= check("formato del enunciado",
                toJsonLine(created) ==
                    R"({"t": 15230, "event": "orderCreated", "order": "o17", "restaurant": "r3"})");

    // 2. Escape: comillas, barra y salto de línea no rompen el JSON ni la línea.
    Event odd;
    odd.event = "orderRejected";
    odd.order = "o\"1\\\n";
    odd.reason = "queueFull";
    ok &= check("escape de caracteres especiales",
                toJsonLine(odd) == R"({"t": 0, "event": "orderRejected", "order": "o\"1\\\n", "reason": "queueFull"})");

    // 3. Concurrencia: kWriters hilos escriben a la vez en el mismo registro.
    std::ostringstream out;  // vive hasta después de los join
    {
        EventLog log(out);
        std::vector<std::thread> writers;
        writers.reserve(kWriters);
        for (int w = 0; w < kWriters; ++w) {
            writers.emplace_back([&log, w] {  // w por valor: cada hilo con su propio número
                for (int i = 0; i < kEventsPerWriter; ++i) {
                    log.write(makeEvent(w, i));
                }
            });
        }
        for (std::thread& writer : writers) {
            writer.join();
        }
    }

    // Cada línea debe ser exactamente uno de los eventos esperados, y para cada hilo
    // deben aparecer en orden: 0, 1, 2, ... (next[w] es el próximo que se espera).
    std::map<std::string, std::pair<int, int>> expected;  // línea -> (hilo, índice)
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kEventsPerWriter; ++i) {
            expected.emplace(toJsonLine(makeEvent(w, i)), std::make_pair(w, i));
        }
    }
    std::vector<int> next(kWriters, 0);
    int lines = 0;
    bool wellFormed = true;
    bool inOrder = true;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line); ++lines) {
        const auto it = expected.find(line);
        if (it == expected.end()) {
            wellFormed = false;  // línea mezclada, cortada o desconocida
            continue;
        }
        const int w = it->second.first;
        inOrder = inOrder && it->second.second == next[w];
        next[w] = it->second.second + 1;
        expected.erase(it);  // así una línea repetida ya no se encuentra
    }

    std::cout << "líneas escritas: " << lines << " de " << kWriters * kEventsPerWriter << "\n";
    ok &= check("cada línea es un evento completo", wellFormed);
    ok &= check("todos los eventos, una sola vez", expected.empty() && lines == kWriters * kEventsPerWriter);
    ok &= check("orden de cada hilo conservado", inOrder);
    return ok ? 0 : 1;
}
