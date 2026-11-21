#!/usr/bin/env python3
"""Escribe la zona de la configuración de ciudad (EC2): Santa Cruz de la Sierra en un círculo de 3 km alrededor de
la plaza 24 de Septiembre (hasta entre el 3er y el 4to anillo), para tools/osm_network.py.

La red es real (OpenStreetMap). Los restaurantes no: son puntos de una grilla de 800 m dentro del círculo, cada uno
en la intersección más cercana, con puestos y preparación que rotan entre los tres niveles de equipetrol.json.
La flota está en la escala que pide el documento de EC2 (2000-5000 motos).

Uso:
  tools/zona_ciudad.py > tools/zona_santa_cruz.json
  tools/osm_map.py --north -17.7534 --south -17.8134 --west -63.2134 --east -63.1508 --zoom 15 \
                   --out data/santa_cruz.png
  tools/osm_network.py --zone tools/zona_santa_cruz.json --image data/santa_cruz.png \
                       --out config/santa_cruz.json --preview-dir docs/red_ciudad --cache ~/.cache/osm_santacruz.json
"""
import json
import math

TILE = 256
ZOOM = 15
BOUNDS = {"north": -17.7533869, "south": -17.8134173, "west": -63.2134008, "east": -63.1507874}  # de osm_map.py
CENTER = (-17.7834, -63.1821)  # plaza 24 de Septiembre
RADIUS_M = 3000
GRID_M = 800
PREP_LEVELS = [[60000, 120000], [120000, 240000], [240000, 420000]]
SLOTS = [1, 2, 2, 3]


def world(lat, lon):
    n = TILE * 2 ** ZOOM
    s = math.sin(math.radians(lat))
    return (lon + 180) / 360 * n, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n


def to_px(lat, lon):
    ox, oy = world(BOUNDS["north"], BOUNDS["west"])
    x, y = world(lat, lon)
    return [round(x - ox, 1), round(y - oy, 1)]


def offset(dy_m, dx_m):
    """El punto a dy_m metros al norte y dx_m al este del centro."""
    lat = CENTER[0] + dy_m / 111320.0
    lon = CENTER[1] + dx_m / (111320.0 * math.cos(math.radians(CENTER[0])))
    return lat, lon


polygon = [to_px(*offset(RADIUS_M * math.sin(a), RADIUS_M * math.cos(a)))
           for a in (2 * math.pi * k / 64 for k in range(64))]
restaurants = []
steps = int(RADIUS_M // GRID_M)
for i in range(-steps, steps + 1):
    for j in range(-steps, steps + 1):
        dy, dx = i * GRID_M, j * GRID_M
        if math.hypot(dx, dy) > RADIUS_M - 300:  # que la intersección más cercana quede adentro
            continue
        k = len(restaurants)
        restaurants.append({"id": "r%d" % k, "name": "Cocina %02d" % k, "px": to_px(*offset(dy, dx)),
                            "pickupSlots": SLOTS[k % len(SLOTS)], "prepTimeMs": PREP_LEVELS[k % len(PREP_LEVELS)]})

zone = {
    "bounds": BOUNDS,
    "zoom": ZOOM,
    "curveTolerancePx": 2.5,
    "polygon": polygon,
    "restaurants": restaurants,
    "startPx": to_px(*CENTER),
    "config": {
        "map": {"image": "../data/santa_cruz.png", "attribution": "© OpenStreetMap contributors"},
        "fleet": {"couriers": 3000, "bagCapacity": 3, "speedKmh": 30},
        "orders": {"meanIntervalMs": 200, "burstMax": 12, "maxPending": 500, "seed": 42},
        "dispatch": {"quoteTimeoutMs": 200, "acceptTimeoutMs": 600000},
        "incidents": {"breakdownProbability": 0.02},
        "simulation": {"durationS": 3600, "timeScale": 60},
    },
    "views": {},
}
print(json.dumps(zone, ensure_ascii=False, indent=1))
