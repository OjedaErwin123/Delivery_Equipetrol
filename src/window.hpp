#pragma once

#include <memory>
#include <string>

#include "config.hpp"
#include "simulation.hpp"
#include "travel.hpp"

// La ventana (sección 6.4): la imagen del mapa con los restaurantes, cada moto como un punto que se
// mueve por las calles (con el color de su estado), el punto de entrega de cada pedido activo, un
// panel con los contadores en vivo y la atribución del mapa. Vive en el hilo principal: la crea
// main y la dibuja el bucle de frames de runSimulation; ningún otro hilo la toca.
//
// OpenCV se usa SOLO para la ventana, y sin su paralelismo interno (cv::setNumThreads(0)): dibuja en
// el hilo que la llama y no crea hilos propios.
class MapWindow {
public:
    // Abre la ventana, o devuelve nullptr y explica por qué en `whyNot` (sin pantalla, o la imagen del
    // mapa no se puede leer). Sin pantalla no se llama a OpenCV: su backend gráfico aborta el proceso.
    static std::unique_ptr<MapWindow> open(const Config& config, std::shared_ptr<const World> world,
                                           std::string& whyNot);

    ~MapWindow();
    MapWindow(const MapWindow&) = delete;
    MapWindow& operator=(const MapWindow&) = delete;

    // Dibuja un frame. false si el usuario cerró la ventana (la simulación se detiene).
    bool draw(const Frame& frame);

private:
    struct Impl;  // los tipos de OpenCV quedan en window.cpp
    explicit MapWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};
