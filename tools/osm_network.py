#!/usr/bin/env python3
"""Extrae la red vial (nodos, segmentos, restaurantes) desde datos de OpenStreetMap.

Lee la zona de simulación de un archivo JSON (bounds de la imagen, polígono, restaurantes
y vistas de revisión; posiciones en píxeles de la imagen de fondo) y produce:
  - un JSON con `nodes`, `streets` y `restaurants` en el formato de la configuración;
  - imágenes de revisión con la red dibujada sobre el mapa.

Pasos: calles transitables de Overpass -> cortes en intersecciones (nodo de OSM compartido
por dos o más calles) -> tramos enteramente dentro del polígono -> componente
fuertemente conexa más grande (respetando sentido único, nadie queda atrapado) -> nodos de
curva con Douglas-Peucker para que ningún segmento recto cruce una manzana -> cada
restaurante en la intersección más cercana a su punto.

Uso:
  tools/osm_network.py --zone tools/zona_equipetrol.json --image data/equipetrol.png \
                       --out red_vial.json --preview-dir docs/red_vial
"""
import argparse, collections, json, math, os, urllib.parse, urllib.request
from PIL import Image, ImageDraw

TILE = 256
UA = "EquipetrolDelivery/0.1 (student project; road network extraction)"
OVERPASS = "https://overpass-api.de/api/interpreter"
DRIVABLE = {"trunk", "primary", "secondary", "tertiary", "unclassified", "residential", "living_street",
            "trunk_link", "primary_link", "secondary_link", "tertiary_link"}
ONEWAY = {"yes", "true", "1", "-1"}


class Projection:
    """Web Mercator: lat/lon <-> píxel de la imagen de fondo (igual que tools/osm_map.py)."""
    def __init__(self, bounds, zoom):
        self.n = TILE * 2 ** zoom
        self.ox, self.oy = self._world(bounds["north"], bounds["west"])

    def _world(self, lat, lon):
        s = math.sin(math.radians(lat))
        return (lon + 180) / 360 * self.n, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * self.n

    def to_px(self, lat, lon):
        x, y = self._world(lat, lon)
        return x - self.ox, y - self.oy


def inside(pt, poly):
    x, y = pt
    c = False
    for (x1, y1), (x2, y2) in zip(poly, poly[-1:] + poly[:-1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def fetch_osm(b, cache):
    if os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)
    q = '[out:json][timeout:90];way["highway"](%f,%f,%f,%f);(._;>;);out body;' % (
        b["south"], b["west"], b["north"], b["east"])
    req = urllib.request.Request(OVERPASS, data=urllib.parse.urlencode({"data": q}).encode(),
                                 headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.load(resp)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "w") as f:
        json.dump(data, f)
    return data


def douglas_peucker(pts, tol):
    """Índices de los puntos que hay que conservar para desviarse menos de `tol` px."""
    keep, stack = {0, len(pts) - 1}, [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        (ax, ay), (bx, by) = pts[a], pts[b]
        length = math.hypot(bx - ax, by - ay) or 1e-9
        best, bi = 0.0, -1
        for i in range(a + 1, b):
            d = abs((bx - ax) * (ay - pts[i][1]) - (ax - pts[i][0]) * (by - ay)) / length
            if d > best:
                best, bi = d, i
        if best > tol:
            keep.add(bi)
            stack += [(a, bi), (bi, b)]
    return sorted(keep)


def largest_scc(edges):
    """Vértices de la componente fuertemente conexa más grande (Kosaraju iterativo)."""
    g, gr = collections.defaultdict(list), collections.defaultdict(list)
    for a, b, oneway in edges:
        g[a].append(b); gr[b].append(a)
        if not oneway:
            g[b].append(a); gr[a].append(b)
    order, seen = [], set()
    for s in list(g) + list(gr):
        if s in seen:
            continue
        seen.add(s)
        stack = [(s, iter(g[s]))]
        while stack:
            v, it = stack[-1]
            nxt = next((u for u in it if u not in seen), None)
            if nxt is None:
                order.append(v); stack.pop()
            else:
                seen.add(nxt); stack.append((nxt, iter(g[nxt])))
    comp, seen = {}, set()
    for s in reversed(order):
        if s in seen:
            continue
        seen.add(s)
        stack = [s]
        while stack:
            v = stack.pop(); comp[v] = s
            for u in gr[v]:
                if u not in seen:
                    seen.add(u); stack.append(u)
    big = collections.Counter(comp.values()).most_common(1)[0][0]
    return {v for v, c in comp.items() if c == big}


def build(zone, osm, proj):
    poly = [tuple(p) for p in zone["polygon"]]
    coords = {e["id"]: (e["lat"], e["lon"]) for e in osm["elements"] if e["type"] == "node"}
    px = {k: proj.to_px(*v) for k, v in coords.items()}
    ways = [w for w in osm["elements"] if w["type"] == "way" and w["tags"].get("highway") in DRIVABLE
            and w["tags"].get("access") not in ("private", "no")]
    uses = collections.Counter()
    for w in ways:
        for i, n in enumerate(w["nodes"]):
            uses[n] += 2 if i in (0, len(w["nodes"]) - 1) else 1  # extremo de vía = intersección

    pieces = []  # (oneway, [nodos de OSM de intersección a intersección], nombre)
    for w in ways:
        t = w["tags"]
        oneway = t.get("oneway") in ONEWAY or t.get("junction") in ("roundabout", "circular")
        seq = w["nodes"][::-1] if t.get("oneway") == "-1" else w["nodes"]
        start = 0
        for i in range(1, len(seq)):
            if uses[seq[i]] >= 2 or i == len(seq) - 1:
                piece = seq[start:i + 1]
                if all(inside(px[n], poly) for n in piece):
                    pieces.append((oneway, piece, t.get("name", "")))
                start = i

    scc = largest_scc([(p[0], p[-1], ow) for ow, p, _ in pieces])
    pieces = [x for x in pieces if x[1][0] in scc and x[1][-1] in scc]

    nodes, ids, streets = [], {}, []
    def node_id(n):
        if n not in ids:
            ids[n] = "n%d" % len(nodes)
            nodes.append({"id": ids[n], "lat": round(coords[n][0], 7), "lon": round(coords[n][1], 7)})
        return ids[n]
    for oneway, piece, name in pieces:
        chain = [piece[k] for k in douglas_peucker([px[n] for n in piece], zone["curveTolerancePx"])]
        for u, v in zip(chain, chain[1:]):
            streets.append({"id": "s%d" % len(streets), "from": node_id(u), "to": node_id(v),
                            "oneWay": oneway, "name": name})
    crossings = {ids[n] for n in ids if uses[n] >= 2}

    pos = {nd["id"]: proj.to_px(nd["lat"], nd["lon"]) for nd in nodes}
    restaurants = []
    for r in zone["restaurants"]:
        rx, ry = r["px"]
        best = min(sorted(crossings), key=lambda k: math.hypot(pos[k][0] - rx, pos[k][1] - ry))
        restaurants.append({"id": r["id"], "name": r["name"], "node": best})
    return nodes, streets, restaurants, crossings, pos


def draw(image, zone, streets, restaurants, crossings, pos, out_dir):
    img = Image.open(image).convert("RGB")
    d = ImageDraw.Draw(img)
    d.polygon([tuple(p) for p in zone["polygon"]], outline=(0, 0, 0))
    for s in streets:
        (x1, y1), (x2, y2) = pos[s["from"]], pos[s["to"]]
        d.line([(x1, y1), (x2, y2)], fill=(220, 40, 40) if s["oneWay"] else (30, 90, 220), width=3)
        if s["oneWay"]:  # flecha en el medio del segmento
            mx, my, a = (x1 + x2) / 2, (y1 + y2) / 2, math.atan2(y2 - y1, x2 - x1)
            d.polygon([(mx + 6 * math.cos(a + da), my + 6 * math.sin(a + da)) for da in (0, 2.5, -2.5)],
                      fill=(120, 0, 0))
    for k, (x, y) in pos.items():
        r = 4 if k in crossings else 2
        d.ellipse([x - r, y - r, x + r, y + r], fill=(0, 0, 0) if k in crossings else (110, 110, 110))
    for r, z in zip(restaurants, zone["restaurants"]):
        x, y = pos[r["node"]]
        d.rectangle([x - 7, y - 7, x + 7, y + 7], fill=(0, 160, 60), outline=(0, 0, 0))
        d.text((x + 9, y - 6), r["id"], fill=(0, 0, 0))
        zx, zy = z["px"]
        d.ellipse([zx - 3, zy - 3, zx + 3, zy + 3], outline=(0, 160, 60), width=2)
    os.makedirs(out_dir, exist_ok=True)
    img.save(os.path.join(out_dir, "red_vial_completa.png"), optimize=True)
    for name, box in zone["views"].items():
        c = img.crop(tuple(box))
        c.resize((c.width * 2, c.height * 2), Image.LANCZOS).save(
            os.path.join(out_dir, "red_vial_%s.png" % name), optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zone", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview-dir", required=True)
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "osm_network.json"))
    a = ap.parse_args()
    with open(a.zone) as f:
        zone = json.load(f)
    proj = Projection(zone["bounds"], zone["zoom"])
    nodes, streets, restaurants, crossings, pos = build(zone, fetch_osm(zone["bounds"], a.cache), proj)
    with open(a.out, "w") as f:
        json.dump({"nodes": nodes, "streets": streets, "restaurants": restaurants}, f, ensure_ascii=False, indent=1)
    draw(a.image, zone, streets, restaurants, crossings, pos, a.preview_dir)
    print("intersecciones %d · nodos %d · segmentos %d (un sentido %d) · restaurantes %d" % (
        len(crossings), len(nodes), len(streets), sum(s["oneWay"] for s in streets), len(restaurants)))
    for r in restaurants:
        names = sorted({s["name"] for s in streets if r["node"] in (s["from"], s["to"]) and s["name"]})
        print("  %s %-10s %s  %s" % (r["id"], r["name"], r["node"], " y ".join(names)))


if __name__ == "__main__":
    main()
