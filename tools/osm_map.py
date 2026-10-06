#!/usr/bin/env python3
"""Genera la imagen de fondo del mapa a partir de los tiles estándar de OpenStreetMap.

Baja los tiles que cubren un rectángulo geográfico, los une y recorta la imagen
justo en ese rectángulo. Imprime los bounds exactos de la imagen resultante
(los bordes de píxel recalculados a lat/lon), listos para `map.bounds`.

Los tiles usan proyección Web Mercator: la longitud es lineal en x, pero la
latitud no es lineal en y. La conversión lat/lon -> píxel del programa debe usar
la misma proyección.

Uso:
  tools/osm_map.py --north N --south S --west W --east E --zoom Z --out data/equipetrol.png

Política de uso de tiles de OSM: User-Agent identificable, caché local y pocas
descargas (aquí, unas decenas de tiles una sola vez).
https://operations.osmfoundation.org/policies/tiles/
"""
import argparse
import io
import json
import math
import os
import sys
import time
import urllib.request

from PIL import Image

TILE = 256
URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "EquipetrolDelivery/0.1 (student project; map background generator)"


def lonlat_to_px(lon, lat, z):
    """Píxel global (x, y) en el mundo Web Mercator de nivel z."""
    n = TILE * 2 ** z
    x = (lon + 180.0) / 360.0 * n
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n
    return x, y


def px_to_lonlat(x, y, z):
    n = TILE * 2 ** z
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lon, lat


def fetch_tile(z, x, y, cache_dir):
    path = os.path.join(cache_dir, str(z), str(x), "%d.png" % y)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        req = urllib.request.Request(URL.format(z=z, x=x, y=y), headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
        with open(path, "wb") as f:
            f.write(data)
        time.sleep(0.2)  # no saturar el servidor de tiles
    return Image.open(path).convert("RGB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--north", type=float, required=True)
    ap.add_argument("--south", type=float, required=True)
    ap.add_argument("--west", type=float, required=True)
    ap.add_argument("--east", type=float, required=True)
    ap.add_argument("--zoom", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "osm_tiles"))
    a = ap.parse_args()
    if not (a.north > a.south and a.east > a.west):
        sys.exit("bounds inválidos: se requiere north > south y east > west")

    # Rectángulo en píxeles globales, ajustado a píxeles enteros.
    x0, y0 = lonlat_to_px(a.west, a.north, a.zoom)
    x1, y1 = lonlat_to_px(a.east, a.south, a.zoom)
    x0, y0, x1, y1 = math.floor(x0), math.floor(y0), math.ceil(x1), math.ceil(y1)

    tx0, ty0, tx1, ty1 = x0 // TILE, y0 // TILE, (x1 - 1) // TILE, (y1 - 1) // TILE
    count = (tx1 - tx0 + 1) * (ty1 - ty0 + 1)
    print("zoom %d: %d tiles, imagen %dx%d px" % (a.zoom, count, x1 - x0, y1 - y0), file=sys.stderr)

    mosaic = Image.new("RGB", ((tx1 - tx0 + 1) * TILE, (ty1 - ty0 + 1) * TILE))
    for tx in range(tx0, tx1 + 1):
        for ty in range(ty0, ty1 + 1):
            mosaic.paste(fetch_tile(a.zoom, tx, ty, a.cache), ((tx - tx0) * TILE, (ty - ty0) * TILE))

    ox, oy = tx0 * TILE, ty0 * TILE
    mosaic.crop((x0 - ox, y0 - oy, x1 - ox, y1 - oy)).save(a.out, optimize=True)

    # Bounds exactos de la imagen recortada (bordes de píxel enteros).
    west, north = px_to_lonlat(x0, y0, a.zoom)
    east, south = px_to_lonlat(x1, y1, a.zoom)
    print(json.dumps({"north": round(north, 7), "south": round(south, 7),
                      "west": round(west, 7), "east": round(east, 7)}))


if __name__ == "__main__":
    main()
