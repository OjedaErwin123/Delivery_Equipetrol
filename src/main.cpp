// delivery_sim: flujo básico de la simulación (v0). Parámetros fijos en el código;
// todavía sin mapa, restaurantes con puestos, estimaciones, averías ni ventana.
#include <fstream>
#include <iostream>

#include "simulation.hpp"

int main()
{
    std::ofstream log("events.log");
    if (!log) {
        std::cerr << "delivery_sim: no se pudo abrir events.log para escribir\n";
        return 1;
    }
    // Fin por tiempo: 80 min simulados (~80 s reales con timeScale 60). A un pedido cada
    // 36 s en promedio salen ~133, menos que `orders`: se detiene con pedidos pendientes.
    SimParams params;
    params.durationS = 4800;
    const Summary summary = runSimulation(params, log, std::cout);
    std::cout << toJson(summary) << "\n";  // última línea de stdout
    return 0;
}
