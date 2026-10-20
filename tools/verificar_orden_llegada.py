#!/usr/bin/env python3
"""Verifica, mirando solo el log, que los restaurantes atienden en orden de llegada.

Uso: python3 tools/verificar_orden_llegada.py <events.log>

Cada pickupStarted trae `arrivedAt`: cuándo llegó la moto al restaurante (el restaurante la anota
con el mismo lock con que le da el ticket de la fila). Una visita es (moto, restaurante, t de
pickupStarted); con la mochila, una moto recoge varios pedidos en la misma visita.

Reglas, por restaurante:
  1. todo pickupStarted trae arrivedAt, y arrivedAt <= t (nadie toma el puesto antes de llegar);
  2. una visita tiene una sola hora de llegada;
  3. ordenadas por arrivedAt, las horas de inicio nunca bajan: quien llegó antes no toma el
     puesto después que otro que llegó más tarde.

Sale con 0 si se cumplen todas, con 1 si no (y lista las violaciones).
"""
import collections
import json
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    visits = {}  # (moto, restaurante, t de inicio) -> horas de llegada vistas
    problems = []
    with open(sys.argv[1], encoding="utf-8") as log:
        for number, line in enumerate(log, 1):
            e = json.loads(line)
            if e["event"] != "pickupStarted":
                continue
            arrived = e.get("arrivedAt")
            if not isinstance(arrived, int) or arrived > e["t"]:
                problems.append(f"línea {number}: arrivedAt ausente o mayor que t: {line.strip()}")
                continue
            visits.setdefault((e["courier"], e["restaurant"], e["t"]), set()).add(arrived)

    per_restaurant = collections.defaultdict(list)
    for (courier, restaurant, start), arrivals in visits.items():
        if len(arrivals) > 1:
            problems.append(f"{restaurant}: la visita de {courier} en t={start} tiene varias llegadas {sorted(arrivals)}")
        per_restaurant[restaurant].append((min(arrivals), start, courier))

    waited = 0
    longest = 0
    for restaurant, vs in sorted(per_restaurant.items()):
        vs.sort()
        for (arr_a, start_a, a), (arr_b, start_b, b) in zip(vs, vs[1:]):
            if start_a > start_b:
                problems.append(f"{restaurant}: {a} llegó en {arr_a} y empezó en {start_a}, pero {b} llegó "
                                f"después ({arr_b}) y empezó antes ({start_b})")
        for arrived, start, _ in vs:
            waited += arrived < start
            longest = max(longest, start - arrived)

    total = sum(len(vs) for vs in per_restaurant.values())
    print(f"visitas: {total} en {len(per_restaurant)} restaurantes; hicieron fila: {waited}; "
          f"mayor espera: {longest} ms simulados")
    for p in problems:
        print("FALLA:", p)
    print("orden de llegada:", "OK" if not problems else f"FALLA ({len(problems)})")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
