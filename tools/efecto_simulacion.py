#!/usr/bin/env python3
"""EC2, sección 2.2 ("The simulation itself"): el efecto de la GPU en la simulación completa.

Corre delivery_sim varias veces (sin ventana, con --frames) y, de cada corrida, saca del log, del resumen y de stderr:
  - la latencia de despacho por pedido: t(primer orderAssigned) - t(orderCreated), en ms simulados (y reales:
    divididos por timeScale);
  - los pedidos asignados por segundo real y los entregados por hora simulada;
  - el tiempo real hasta "Simulation has started..." (arranque: configuración + alcanzabilidad, T1 en la GPU);
  - los frames por segundo del bucle de frames y sus intervalos (--frames);
  - las veces que el prefiltro de R1 en la GPU no respondió o falló y se hizo en la CPU;
  - si el resumen es consistente con el log (created = delivered + rejected + pending, y cada contador = sus líneas).

Solo la biblioteca estándar (corre en la laptop y en Colab).

Uso:
  efecto_simulacion.py run <delivery_sim> <config.json> <versión> <repeticiones> <carpeta> [primera]
      agrega una fila por corrida a <carpeta>/simulacion.csv (y deja el log de la repetición 0, comprimido);
      [primera] = el número de la primera repetición (para intercalar versiones de a una corrida)
  efecto_simulacion.py table <simulacion.csv> [...]
      una tabla en Markdown: mediana [mín–máx] por máquina y versión
"""
import csv
import gzip
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

COLUMNS = [
    "machine", "version", "repetition", "real_s", "startup_ms", "created", "assigned", "delivered", "rejected",
    "queueFull", "noCourier", "unreachable", "pending", "dispatch_p50_sim_ms", "dispatch_p90_sim_ms",
    "dispatch_mean_sim_ms", "dispatch_p50_real_ms", "assigned_per_real_s", "delivered_per_sim_hour", "frames_per_s",
    "frame_p50_ms", "frame_p99_ms", "frame_max_ms", "frames_over_50ms", "gpu_fallbacks", "consistent",
]


def machine():
    cpu = platform.processor() or platform.machine()
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    name = f"{cpu} x{os.cpu_count()}"
    if shutil.which("nvidia-smi"):
        gpu = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"], capture_output=True,
                             text=True).stdout.strip()
        if gpu:
            name += f" | {gpu}"
    return name


def percentile(values, q):
    """Vecino más cercano, como FrameMeter: la posición ceil(q * n) de los valores ordenados."""
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return float(ordered[index])


def analyze_log(path):
    created, assigned_at, counts, reasons = {}, {}, {}, {}
    end = 0
    with open(path) as f:
        for line in f:
            e = json.loads(line)
            kind = e["event"]
            counts[kind] = counts.get(kind, 0) + 1
            end = max(end, e["t"])
            if kind == "orderCreated":
                created[e["order"]] = e["t"]
            elif kind == "orderAssigned" and e["order"] not in assigned_at:
                assigned_at[e["order"]] = e["t"]
            elif kind == "orderRejected":
                reasons[e["reason"]] = reasons.get(e["reason"], 0) + 1
    latencies = [assigned_at[o] - created[o] for o in assigned_at]
    return latencies, counts, reasons, end


def frames_from(stderr):
    """La línea de --frames: 'frames: N, por segundo F, intervalo mediana A ms, p99 B ms, máximo C ms, de más de
    50 ms: K'."""
    for line in stderr.splitlines():
        if "frames: " in line and "por segundo" in line:
            part = line.split("frames: ", 1)[1]
            numbers = []
            for token in part.replace(",", " ").split():
                try:
                    numbers.append(float(token))
                except ValueError:
                    pass
            if len(numbers) >= 7:  # N, F, A, B, C, 50, K
                return numbers[1], numbers[2], numbers[3], numbers[4], int(numbers[6])
    return 0.0, 0.0, 0.0, 0.0, 0


def run(binary, config, version, repetitions, folder, first=0):
    os.makedirs(folder, exist_ok=True)
    time_scale = json.load(open(config))["simulation"]["timeScale"]
    out_csv = os.path.join(folder, "simulacion.csv")
    new = not os.path.exists(out_csv)
    name = machine()
    with open(out_csv, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS)
        if new:
            writer.writeheader()
        for r in range(first, first + repetitions):
            log = os.path.join(folder, f"{version}_{r}.log")
            begin = time.monotonic()
            proc = subprocess.Popen([binary, config, "--headless", "--frames", "--log", log], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            started = proc.stdout.readline()  # "Simulation has started..."
            startup_ms = (time.monotonic() - begin) * 1000
            rest, stderr = proc.communicate()
            real_s = time.monotonic() - begin
            if proc.returncode != 0 or not started.startswith("Simulation has started"):
                sys.exit(f"{version} {r}: salida {proc.returncode}\n{stderr}")
            summary = json.loads(rest.strip().splitlines()[-1])
            latencies, counts, reasons, end = analyze_log(log)
            consistent = (summary["created"] == summary["delivered"] + summary["rejected"] + summary["pending"]
                          and summary["created"] == counts.get("orderCreated", 0)
                          and summary["delivered"] == counts.get("orderDelivered", 0)
                          and summary["rejected"] == counts.get("orderRejected", 0))
            fps, f50, f99, fmax, slow = frames_from(stderr)
            p50 = percentile(latencies, 0.5)
            row = {
                "machine": name, "version": version, "repetition": r, "real_s": round(real_s, 2),
                "startup_ms": round(startup_ms, 1), "created": summary["created"],
                "assigned": counts.get("orderAssigned", 0), "delivered": summary["delivered"],
                "rejected": summary["rejected"], "queueFull": reasons.get("queueFull", 0),
                "noCourier": reasons.get("noCourier", 0), "unreachable": reasons.get("unreachable", 0),
                "pending": summary["pending"], "dispatch_p50_sim_ms": p50,
                "dispatch_p90_sim_ms": percentile(latencies, 0.9),
                "dispatch_mean_sim_ms": round(statistics.mean(latencies), 1) if latencies else 0,
                "dispatch_p50_real_ms": round(p50 / time_scale, 2),
                "assigned_per_real_s": round(counts.get("orderAssigned", 0) / real_s, 2),
                "delivered_per_sim_hour": round(summary["delivered"] * 3_600_000 / end, 1) if end else 0,
                "frames_per_s": fps, "frame_p50_ms": f50, "frame_p99_ms": f99, "frame_max_ms": fmax,
                "frames_over_50ms": slow,
                "gpu_fallbacks": sum(1 for line in stderr.splitlines() if "prefiltro en la GPU" in line),
                "consistent": "yes" if consistent else "no",
            }
            writer.writerow(row)
            f.flush()
            print(f"{version} {r}: {row['real_s']} s reales, {row['assigned']} asignados, {row['delivered']} "
                  f"entregados, despacho p50 {p50 / 1000:.1f} s simulados, {fps:.1f} frames/s, "
                  f"consistente: {row['consistent']}", flush=True)
            if r == 0:
                with open(log, "rb") as src, gzip.open(log + ".gz", "wb") as dst:
                    shutil.copyfileobj(src, dst)
                with open(os.path.join(folder, f"{version}_stderr.txt"), "w") as e:
                    e.write(stderr)
            os.remove(log)


def table(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            rows += list(csv.DictReader(f))
    # (columna, título, escala): el despacho se muestra en segundos simulados.
    shown = [("startup_ms", "arranque (ms reales)", 1), ("assigned_per_real_s", "asignados / s real", 1),
             ("dispatch_p50_sim_ms", "despacho p50 (s sim.)", 1000), ("dispatch_p90_sim_ms", "despacho p90 (s sim.)", 1000),
             ("delivered_per_sim_hour", "entregados / h sim.", 1), ("frames_per_s", "frames / s", 1),
             ("frame_p99_ms", "frame p99 (ms)", 1), ("frame_max_ms", "frame máx. (ms)", 1)]
    groups = {}
    for row in rows:
        groups.setdefault((row["machine"], row["version"]), []).append(row)
    print("| máquina | versión | corridas | " + " | ".join(t for _, t, _ in shown) + " | fallbacks | ¿consistente? |")
    print("|---|---|---:|" + "---:|" * len(shown) + "---:|:---:|")
    for (name, version), group in groups.items():
        cells = []
        for key, _, scale in shown:
            values = [float(g[key]) / scale for g in group]
            cells.append(f"{statistics.median(values):.1f} [{min(values):.1f}–{max(values):.1f}]")
        fallbacks = sum(int(g["gpu_fallbacks"]) for g in group)
        ok = "sí" if all(g["consistent"] == "yes" for g in group) else "NO"
        print(f"| {name.replace('|', '+')} | {version} | {len(group)} | " + " | ".join(cells) + f" | {fallbacks} | {ok} |")


if __name__ == "__main__":
    if len(sys.argv) in (7, 8) and sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), sys.argv[6],
            int(sys.argv[7]) if len(sys.argv) == 8 else 0)
    elif len(sys.argv) >= 3 and sys.argv[1] == "table":
        table(sys.argv[2:])
    else:
        sys.exit(__doc__)
