#include "window.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "projection.hpp"

namespace {

const char* const kTitle = "Equipetrol Delivery";

// Colores (BGR), como la ilustración del enunciado.
const cv::Scalar kIdle(140, 140, 140);         // gris: inactiva
const cv::Scalar kToRestaurant(230, 120, 20);  // azul: rumbo a un restaurante
const cv::Scalar kCarrying(40, 170, 40);       // verde: llevando pedidos
const cv::Scalar kBroken(30, 30, 220);         // rojo: averiada
const cv::Scalar kRestaurant(20, 20, 20);      // cuadrados negros
const cv::Scalar kDelivery(150, 40, 150);      // círculos huecos: puntos de entrega
const cv::Scalar kText(20, 20, 20);
const cv::Scalar kWhite(255, 255, 255);

cv::Scalar colorOf(CourierStatus status)
{
    switch (status) {
        case CourierStatus::idle:
            return kIdle;
        case CourierStatus::toRestaurant:
            return kToRestaurant;
        case CourierStatus::carrying:
            return kCarrying;
        case CourierStatus::broken:
            return kBroken;
    }
    return kIdle;
}

void text(cv::Mat& img, const std::string& s, cv::Point at, double scale = 0.45)
{
    cv::putText(img, s, at, cv::FONT_HERSHEY_SIMPLEX, scale, kText, 1, cv::LINE_AA);
}

std::string minutes(std::int64_t ms)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f min", static_cast<double>(ms) / 60000.0);
    return buf;
}

std::string hms(std::int64_t ms)
{
    const std::int64_t s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld", static_cast<long long>(s / 3600),
                  static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
    return buf;
}

bool hasDisplay()
{
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
}

}  // namespace

struct MapWindow::Impl {
    cv::Mat map;
    MapBounds bounds;
    std::string attribution;
    std::shared_ptr<const World> world;
    std::vector<cv::Point> restaurants;

    cv::Point pixel(const Coordinates& c) const
    {
        const Pixel p = toPixel(bounds, map.cols, map.rows, c);
        return {cvRound(p.x), cvRound(p.y)};
    }
};

std::unique_ptr<MapWindow> MapWindow::open(const Config& config, std::shared_ptr<const World> world,
                                           std::string& whyNot)
{
    if (!hasDisplay()) {
        whyNot = "no hay pantalla (DISPLAY y WAYLAND_DISPLAY vacías)";
        return nullptr;
    }
    cv::setNumThreads(0);  // OpenCV sin hilos propios: todo en el hilo que dibuja
    auto impl = std::make_unique<Impl>();
    impl->map = cv::imread(config.mapImage, cv::IMREAD_COLOR);
    if (impl->map.empty()) {
        whyNot = "no se pudo leer la imagen del mapa '" + config.mapImage + "'";
        return nullptr;
    }
    impl->bounds = {config.north, config.south, config.west, config.east};
    impl->attribution = config.attribution;
    impl->world = std::move(world);
    for (const auto& restaurant : impl->world->restaurantNode) {
        impl->restaurants.push_back(impl->pixel(impl->world->graph.coordinates(restaurant.second)));
    }
    cv::namedWindow(kTitle, cv::WINDOW_AUTOSIZE);
    return std::unique_ptr<MapWindow>(new MapWindow(std::move(impl)));
}

MapWindow::MapWindow(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

MapWindow::~MapWindow()
{
    // Valgrind marca un "Invalid read" DENTRO de OpenCV (su backend de Qt) al destruir la ventana;
    // sin destruirla desaparece, pero quedan ~2,8 MB "possibly lost". Se destruye (ver README, DP.11).
    cv::destroyWindow(kTitle);
}

bool MapWindow::draw(const Frame& frame)
{
    const Impl& w = *_impl;
    const RoadGraph& graph = w.world->graph;
    cv::Mat img = w.map.clone();

    for (const cv::Point& r : w.restaurants) {
        cv::rectangle(img, r - cv::Point(6, 6), r + cv::Point(6, 6), kRestaurant, cv::FILLED);
    }
    for (const auto& order : frame.stats.activeDestinations) {
        if (const std::optional<std::size_t> node = graph.nodeIndex(order.second)) {
            cv::circle(img, w.pixel(graph.coordinates(*node)), 6, kDelivery, 2, cv::LINE_AA);
        }
    }
    // Cada moto en su posición de ESTE instante: una función pura de su viaje (Trip::positionAt).
    for (const CourierView& c : frame.couriers) {
        const cv::Point at = w.pixel(c.trip.positionAt(graph, frame.nowMs));
        cv::circle(img, at, 7, kWhite, cv::FILLED, cv::LINE_AA);
        cv::circle(img, at, 5, colorOf(c.status), cv::FILLED, cv::LINE_AA);
    }

    // Panel con los contadores en vivo (los mismos que el resumen final, tal como están en el log).
    const LiveStats& s = frame.stats;
    cv::Mat panel = img(cv::Rect(0, 0, std::min(img.cols, 430), std::min(img.rows, 150)));
    cv::Mat shade(panel.size(), panel.type(), kWhite);
    cv::addWeighted(panel, 0.25, shade, 0.75, 0, panel);
    text(img, "t = " + hms(frame.nowMs) + " (simulado)", {10, 20}, 0.5);
    text(img, "creados " + std::to_string(s.created) + "   entregados " + std::to_string(s.delivered) +
                  "   rechazados " + std::to_string(s.rejected) + "   pendientes " + std::to_string(s.pending()),
         {10, 44});
    text(img, "reasignados " + std::to_string(s.reassigned) + "   averias " + std::to_string(s.breakdowns), {10, 66});
    text(img, "entrega media " + minutes(s.avgDeliveryMs()) + "   maxima " + minutes(s.maxDeliveryMs), {10, 88});
    const std::pair<cv::Scalar, const char*> legend[] = {
        {kIdle, "inactiva"}, {kToRestaurant, "a restaurante"}, {kCarrying, "llevando"}, {kBroken, "averiada"}};
    int x = 16;
    for (const auto& item : legend) {
        cv::circle(img, {x, 116}, 5, item.first, cv::FILLED, cv::LINE_AA);
        text(img, item.second, {x + 10, 120}, 0.4);
        int baseline = 0;
        x += 10 + cv::getTextSize(item.second, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseline).width + 22;
    }
    cv::rectangle(img, {10, 132}, {20, 142}, kRestaurant, cv::FILLED);
    text(img, "restaurante", {26, 142}, 0.4);
    cv::circle(img, {120, 137}, 5, kDelivery, 2, cv::LINE_AA);
    text(img, "punto de entrega", {130, 142}, 0.4);

    // Atribución abajo a la derecha. Las fuentes de OpenCV no tienen "©": se dibuja con un círculo.
    std::string credit = w.attribution;
    if (credit.compare(0, 2, "\xC2\xA9") == 0) {
        credit = credit.substr(2);  // el © en UTF-8 son dos bytes
    }
    int baseline = 0;
    const cv::Size size = cv::getTextSize(credit, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
    const cv::Point origin(img.cols - size.width - 10, img.rows - 10);
    cv::rectangle(img, origin + cv::Point(-22, -size.height - 6), {img.cols, img.rows}, kWhite, cv::FILLED);
    cv::circle(img, origin + cv::Point(-12, -size.height / 2), 7, kText, 1, cv::LINE_AA);
    text(img, "c", origin + cv::Point(-16, -size.height / 2 + 4), 0.38);
    text(img, credit, origin);

    cv::imshow(kTitle, img);
    cv::waitKey(1);  // procesa los eventos de la ventana (no es para esperar: 1 ms)
    return cv::getWindowProperty(kTitle, cv::WND_PROP_VISIBLE) >= 1;
}
