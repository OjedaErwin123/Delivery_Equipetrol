// Prueba de los lugares a mitad de calle (donde quedará una moto averiada): un viaje dice en qué
// punto de qué calle va en cada instante; desde un punto de una calle se sale respetando el sentido
// único (por una de sentido único, solo hacia adelante); un viaje y una estimación pueden salir
// de ahí; y la distancia por calle HASTA un punto de una calle (con la que se elige quién va a
// buscar a una moto averiada). Sale con 0 si todo está bien.
#include <cmath>
#include <iostream>
#include <memory>

#include "estimate.hpp"
#include "log_line.hpp"
#include "restaurant.hpp"
#include "travel.hpp"

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

// a→b→c de sentido único (sobre lat 0); a–d–c de doble sentido (por d = (0,001; 0,001)). r0 en b.
Config map()
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}};
    return c;
}

}  // namespace

int main()
{
    bool ok = true;
    const RoadGraph g(map());
    const std::size_t a = *g.nodeIndex("a"), b = *g.nodeIndex("b"), cc = *g.nodeIndex("c"), d = *g.nodeIndex("d");
    const double ab = g.streetMeters(a, b), bc = g.streetMeters(b, cc), ad = g.streetMeters(a, d);

    // 1. Un viaje a→b→c a 1 m/ms: en cada instante, en qué calle y a cuántos metros de su inicio.
    const Trip ac(g, *g.shortestPath(a, cc), 0, 1.0);
    const Place inFirst = ac.placeAt(50);
    const Place inSecond = ac.placeAt(static_cast<std::int64_t>(ab) + 20);
    std::cout << "t=50: " << describe(g, inFirst) << "; t=ab+20: " << describe(g, inSecond) << "\n";
    ok &= check("a los 50 ms va por a→b, a 50 m de a", inFirst.a == a && inFirst.b == b && near(inFirst.meters, 50));
    ok &= check("pasado b va por b→c", inSecond.a == b && inSecond.b == cc &&
                                           near(inSecond.meters, static_cast<std::int64_t>(ab) + 20 - ab, 1e-6));
    ok &= check("al salir es un nodo (a) y al llegar otro (c)",
                ac.placeAt(0) == Place(a) && ac.placeAt(ac.arrivalMs()) == Place(cc) && ac.placeAt(0).isNode());
    const Coordinates p = coordinatesOf(g, inFirst);
    ok &= check("sus coordenadas están sobre la calle a→b", near(p.lat, 0) && near(p.lon, 0.001 * 50 / ab, 1e-12));
    ok &= check("una moto quieta a mitad de calle se dibuja ahí",
                near(Trip::stay(inFirst, 0).positionAt(g, 1e9).lon, p.lon));

    // 2. Desde 30 m de a sobre a→b (sentido único) a `a`: no puede volver por a→b; sale por b y da
    //    la vuelta por c y d.
    const std::optional<Route> oneWay = routeTo(g, Place(a, b, 30), a);
    ok &= check("sentido único: sale por b, hacia adelante",
                oneWay && oneWay->path.nodes.front() == b &&
                    near(oneWay->meters, (ab - 30) + bc + g.streetMeters(cc, d) + ad, 1e-6));

    // 3. Desde 30 m de a sobre a–d (doble sentido): a `a`, vuelve 30 m; a c gana el más corto
    //    (hacia atrás por a→b→c, o hacia adelante por d→c).
    const Place onTwoWay(a, d, 30);
    const std::optional<Route> back = routeTo(g, onTwoWay, a);
    ok &= check("doble sentido: puede volver hacia atrás (30 m hasta a)",
                back && back->path.nodes.front() == a && near(back->meters, 30, 1e-6));
    const std::optional<Route> toC = routeTo(g, onTwoWay, cc);
    const double backward = 30 + ab + bc, forward = (ad - 30) + g.streetMeters(d, cc);
    ok &= check("doble sentido: elige el lado más corto", toC && near(toC->meters, std::min(backward, forward), 1e-6));

    // 4. Un viaje que sale de ese punto: dura lo que falta / velocidad y va por la calle hacia atrás.
    const Trip fromSpot(g, *back, 1000, 1.0);
    ok &= check("sale del punto, tarda 30 ms y llega a a",
                fromSpot.arrivalMs() == 1030 && fromSpot.from() == onTwoWay && fromSpot.to() == Place(a));
    const Coordinates start = fromSpot.positionAt(g, 1000), mid = fromSpot.positionAt(g, 1015);
    const Coordinates expectStart = coordinatesOf(g, onTwoWay), expectMid = coordinatesOf(g, Place(a, d, 15));
    ok &= check("arranca en el punto y a los 15 ms está a 15 m de a, sobre la misma calle",
                near(start.lat, expectStart.lat) && near(start.lon, expectStart.lon) &&
                    near(mid.lat, expectMid.lat, 1e-9) && near(mid.lon, expectMid.lon, 1e-9));

    // 5. Una estimación desde una moto quieta a mitad de calle: el mismo recorrido que hará.
    auto world = std::make_shared<World>(World{g, {{"r0", b}}, a, 1.0});
    const CourierPlan stopped(Trip::stay(onTwoWay, 0));
    const std::int64_t eta = estimateDelivery(*world, stopped, {"r0", "c", 0, false}, 0);
    const std::int64_t expected = static_cast<std::int64_t>(std::ceil(routeTo(g, onTwoWay, b)->meters)) +
                                  kHandoverMs + static_cast<std::int64_t>(std::ceil(g.shortestPath(b, cc)->meters));
    std::cout << "estimación desde " << describe(g, onTwoWay) << ": " << eta << " (esperado " << expected << ")\n";
    ok &= check("la estimación sale del punto de la calle", eta == expected);

    // 6. Hasta un punto de una calle: se entra por su inicio (o por su final si es de doble
    //    sentido), o directo si se está en la misma calle y el sentido lo permite.
    const double loop = (ab - 50) + bc + g.streetMeters(cc, d) + ad;  // b→c→d→a
    ok &= check("desde a hasta 30 m de a sobre a→b: 30 m", near(*metersBetween(g, Place(a), Place(a, b, 30)), 30, 1e-6));
    ok &= check("en la misma calle, hacia adelante: 20 m",
                near(*metersBetween(g, Place(a, b, 10), Place(a, b, 30)), 20, 1e-6));
    ok &= check("en la misma calle de sentido único pero ya pasado: da la vuelta",
                near(*metersBetween(g, Place(a, b, 50), Place(a, b, 30)), loop + 30, 1e-6));
    ok &= check("calle de doble sentido escrita al revés (d→a): 10 m",
                near(*metersBetween(g, Place(d, a, ad - 20), Place(a, d, 30)), 10, 1e-6));
    return ok ? 0 : 1;
}
