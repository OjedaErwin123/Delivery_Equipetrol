#!/usr/bin/env python3
"""Verifica, mirando solo el log, que los restaurantes atienden en orden de llegada y respetan las pausas.

Uso: python3 tools/verificar_orden_llegada.py <events.log> [salida.txt]

Cada pickupStarted trae `arrivedAt`: cuándo llegó la moto al restaurante (el restaurante la anota
con el mismo lock con que le da el ticket de la fila). Una visita es (moto, restaurante, t de
pickupStarted); con la mochila, una moto recoge varios pedidos en la misma visita.

Reglas, por restaurante:
  1. todo pickupStarted trae arrivedAt, y arrivedAt <= t (nadie toma el puesto antes de llegar);
  2. una visita tiene una sola hora de llegada;
  3. ordenadas por arrivedAt, las horas de inicio nunca bajan: quien llegó antes no toma el
     puesto después que otro que llegó más tarde.

Pausas de cocina (variante B), si el log trae restaurantPaused / restaurantResumed:
  4. por restaurante, pausa y reanudación alternan, y ninguna queda después de simulationStopping;
  5. ningún pickupStarted cae dentro de una pausa [restaurantPaused, restaurantResumed).

Con `salida.txt` (la salida estándar de delivery_sim), si el resumen trae los campos de las pausas:
  6. "pauses" es igual a las líneas restaurantPaused;
  7. "pickupsDelayedByPause", recalculado desde el log, es igual al del resumen: por cada visita,
     una por cada pausa que empezó mientras esperaba (arrivedAt < inicio < t), y una más si llegó
     en plena pausa.

Sale con 0 si se cumplen todas, con 1 si no (y lista las violaciones).
"""
import bisect
import collections
import json
import sys


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    visits = {}  # (moto, restaurante, t de inicio) -> horas de llegada vistas
    problems = []
    pauses = collections.defaultdict(list)  # restaurante -> [[inicio, fin o None], ...]
    paused_lines = 0
    stopped = False
    with open(sys.argv[1], encoding="utf-8") as log:
        for number, line in enumerate(log, 1):
            e = json.loads(line)
            if e["event"] == "simulationStopping":
                stopped = True
            elif e["event"] in ("restaurantPaused", "restaurantResumed"):
                r, starts = e["restaurant"], e["event"] == "restaurantPaused"
                if stopped:
                    problems.append(f"línea {number}: {e['event']} después de simulationStopping")
                is_open = bool(pauses[r]) and pauses[r][-1][1] is None
                if starts == is_open:
                    problems.append(f"línea {number}: {r} no alterna pausa y reanudación: {line.strip()}")
                elif starts:
                    pauses[r].append([e["t"], None])
                    paused_lines += 1
                else:
                    pauses[r][-1][1] = e["t"]
            elif e["event"] == "pickupStarted":
                arrived = e.get("arrivedAt")
                if not isinstance(arrived, int) or arrived > e["t"]:
                    problems.append(f"línea {number}: arrivedAt ausente o mayor que t: {line.strip()}")
                    continue
                visits.setdefault((e["courier"], e["restaurant"], e["t"]), set()).add(arrived)

    def in_pause(r, t):  # ¿t cae en [inicio, fin) de alguna pausa de r? (fin None: siguió hasta el final)
        return any(s <= t and (end is None or t < end) for s, end in pauses.get(r, []))

    per_restaurant = collections.defaultdict(list)
    delayed = 0
    for (courier, restaurant, start), arrivals in visits.items():
        if len(arrivals) > 1:
            problems.append(f"{restaurant}: la visita de {courier} en t={start} tiene varias llegadas {sorted(arrivals)}")
        arrived = min(arrivals)
        per_restaurant[restaurant].append((arrived, start, courier))
        if in_pause(restaurant, start):
            problems.append(f"{restaurant}: {courier} tomó el puesto en t={start}, dentro de una pausa")
        starts = [s for s, _ in pauses.get(restaurant, [])]
        delayed += bisect.bisect_left(starts, start) - bisect.bisect_right(starts, arrived)
        delayed += 1 if in_pause(restaurant, arrived) else 0

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
    if paused_lines:
        print(f"pausas: {paused_lines} restaurantPaused en {len(pauses)} restaurantes; "
              f"pickups demorados por pausa (desde el log): {delayed}")

    if len(sys.argv) == 3:
        with open(sys.argv[2], encoding="utf-8") as out:
            summary = json.loads(out.read().strip().splitlines()[-1])
        if "pauses" in summary:
            if summary["pauses"] != paused_lines:
                problems.append(f"resumen: pauses = {summary['pauses']}, pero hay {paused_lines} restaurantPaused")
            if summary["pickupsDelayedByPause"] != delayed:
                problems.append(f"resumen: pickupsDelayedByPause = {summary['pickupsDelayedByPause']}, "
                                f"pero desde el log son {delayed}")
            print(f"resumen: pauses {summary['pauses']}, pickupsDelayedByPause {summary['pickupsDelayedByPause']}")
        elif paused_lines:
            problems.append("hay eventos de pausa pero el resumen no trae pauses ni pickupsDelayedByPause")

    for p in problems:
        print("FALLA:", p)
    print("orden de llegada y pausas:", "OK" if not problems else f"FALLA ({len(problems)})")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
