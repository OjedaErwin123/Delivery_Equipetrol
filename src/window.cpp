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
// Alto inicial de la ventana: el mapa (1014 px) no entra en una pantalla de 768. Se escala
// manteniendo la proporción, y al agrandarla o maximizarla el mapa la ocupa entera.
const int kInitialHeight = 620;

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

// q, Q o Esc: terminar, como cerrar la ventana.
bool isFinishKey(int key) { return key == 'q' || key == 'Q' || key == 27; }

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
    cv::Mat last;                   // el último frame dibujado (para la pantalla final)
    bool finishPressed = false;     // el botón "Terminar"; lo escribe su callback, en este mismo hilo

    cv::Point pixel(const Coordinates& c) const
    {
        const Pixel p = toPixel(bounds, map.cols, map.rows, c);
        return {cvRound(p.x), cvRound(p.y)};
    }
};

std::unique_ptr<MapWindow> MapWindow::open(const Config& config, std::string& whyNot)
{
    if (!hasDisplay()) {
        whyNot = "no hay pantalla (DISPLAY y WAYLAND_DISPLAY vacías)";
        return nullptr;
    }
    // Dos funciones de Qt que la ventana no usa y que bajo Valgrind dan errores que no son nuestros
    // (README, DP.11): el JIT de expresiones regulares (PCRE2) genera código que Memcheck no puede
    // seguir (18 "Conditional jump" falsos), y la integración con OpenGL carga Mesa, que deja memoria
    // sin liberar (se dibuja una imagen con imshow, sin OpenGL). Sin pisar lo que ya defina el usuario
    // (último argumento 0). setenv no es seguro con otros hilos leyendo el entorno: por eso la ventana
    // se abre antes de crear hilos.
    setenv("QT_ENABLE_REGEXP_JIT", "0", 0);
    setenv("QT_XCB_GL_INTEGRATION", "none", 0);
    cv::setNumThreads(0);  // OpenCV sin hilos propios: todo en el hilo que dibuja
    auto impl = std::make_unique<Impl>();
    impl->map = cv::imread(config.mapImage, cv::IMREAD_COLOR);
    if (impl->map.empty()) {
        whyNot = "no se pudo leer la imagen del mapa '" + config.mapImage + "'";
        return nullptr;
    }
    impl->bounds = {config.north, config.south, config.west, config.east};
    impl->attribution = config.attribution;
    // WINDOW_NORMAL: la imagen se escala al tamaño de la ventana (KEEPRATIO: sin deformarse), y con
    // el backend Qt funcionan los botones de zoom y desplazamiento de su barra (con AUTOSIZE no).
    cv::namedWindow(kTitle, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO | cv::WINDOW_GUI_EXPANDED);
    const double fit = std::min(1.0, static_cast<double>(kInitialHeight) / impl->map.rows);
    cv::resizeWindow(kTitle, cvRound(impl->map.cols * fit), cvRound(impl->map.rows * fit));
    // El botón va en el panel de propiedades de Qt (el último botón de la barra). Otros backends
    // (GTK) no tienen botones: lanzan cv::Exception y quedan las teclas y la X.
    try {
        cv::createButton(
            "Terminar", [](int, void* data) { static_cast<Impl*>(data)->finishPressed = true; }, impl.get(),
            cv::QT_PUSH_BUTTON);
    } catch (const cv::Exception&) {
    }
    return std::unique_ptr<MapWindow>(new MapWindow(std::move(impl)));
}

MapWindow::MapWindow(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

void MapWindow::setWorld(std::shared_ptr<const World> world)
{
    Impl& w = *_impl;
    w.world = std::move(world);
    w.restaurants.clear();
    for (const auto& restaurant : w.world->restaurantNode) {
        w.restaurants.push_back(w.pixel(w.world->graph.coordinates(restaurant.second)));
    }
}

MapWindow::~MapWindow()
{
    // Valgrind marca un "Invalid read" DENTRO de OpenCV al destruir la última ventana: su backend de Qt
    // (window_QT.cpp, también en la rama 4.x actual) borra su objeto GuiReceiver en isLastWindow()
    // mientras todavía ejecuta un método de ese mismo objeto, y después lee un miembro suyo.
    // destroyAllWindows() da lo mismo; sin destruirla quedan ~2,8 MB "possibly lost". Se destruye
    // (ver README, DP.11).
    cv::destroyWindow(kTitle);
}

bool MapWindow::draw(const Frame& frame)
{
    Impl& w = *_impl;
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
    w.last = img;
    // waitKey procesa los eventos de la ventana (no es para esperar: 1 ms), y ahí corre el callback
    // del botón: en este hilo, así que finishPressed no se comparte con nadie.
    const int key = cv::waitKey(1);
    return !isFinishKey(key) && !w.finishPressed && cv::getWindowProperty(kTitle, cv::WND_PROP_VISIBLE) >= 1;
}

void MapWindow::showFinal(const Summary& s, std::chrono::steady_clock::time_point until)
{
    Impl& w = *_impl;
    if (w.last.empty() || cv::getWindowProperty(kTitle, cv::WND_PROP_VISIBLE) < 1) {
        return;
    }
    cv::Mat img = w.last.clone();
    const cv::Rect box(img.cols / 2 - 200, img.rows / 2 - 95, 400, 190);
    cv::Mat area = img(box & cv::Rect(0, 0, img.cols, img.rows));
    cv::Mat shade(area.size(), area.type(), kWhite);
    cv::addWeighted(area, 0.15, shade, 0.85, 0, area);
    cv::rectangle(img, box, kText, 2);
    const int x = box.x + 20;
    text(img, "Simulacion terminada", {x, box.y + 32}, 0.7);
    text(img, "creados " + std::to_string(s.created) + "   entregados " + std::to_string(s.delivered), {x, box.y + 64});
    text(img, "rechazados " + std::to_string(s.rejected) + "   pendientes " +
                  std::to_string(s.created - s.delivered - s.rejected), {x, box.y + 88});
    text(img, "reasignados " + std::to_string(s.reassigned) + "   averias " + std::to_string(s.breakdowns),
         {x, box.y + 112});
    text(img, "entrega media " + minutes(s.avgDeliveryMs) + "   maxima " + minutes(s.maxDeliveryMs), {x, box.y + 136});
    text(img, "(el resumen completo esta en la terminal)", {x, box.y + 170}, 0.4);
    cv::imshow(kTitle, img);
    // Una pausa por frame mientras se muestra (regula los FPS: lo único que la ventana hace aquí);
    // termina antes si se cierra la ventana, se aprieta una tecla o el botón.
    while (std::chrono::steady_clock::now() < until) {
        if (cv::waitKey(33) >= 0 || w.finishPressed || cv::getWindowProperty(kTitle, cv::WND_PROP_VISIBLE) < 1) {
            break;
        }
    }
}
