// MapWindow sin OpenCV (DELIVERY_WINDOW=OFF): open() nunca abre una ventana y explica por qué, así main
// sigue como cuando no hay pantalla (aviso en stderr y simulación sin ventana). Los demás métodos existen
// para que main compile, pero no se llaman: no hay ninguna MapWindow.
#include "window.hpp"

struct MapWindow::Impl {};

std::unique_ptr<MapWindow> MapWindow::open(const Config&, std::string& whyNot)
{
    whyNot = "compilado sin ventana (DELIVERY_WINDOW=OFF)";
    return nullptr;
}

MapWindow::MapWindow(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

MapWindow::~MapWindow() = default;

void MapWindow::setWorld(std::shared_ptr<const World>) {}

bool MapWindow::draw(const Frame&)
{
    return true;
}

void MapWindow::showFinal(const Summary&, std::chrono::steady_clock::time_point) {}
