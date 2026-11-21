#!/usr/bin/env python3
"""EC2, sección 2.4: un roofline de la T4 con el kernel principal de cada aplicación.

Por kernel, en su tamaño medido más grande:
  intensidad = operaciones útiles / bytes mínimos de DRAM (los que el algoritmo tiene que leer y escribir sí o sí),
               contados desde el código (la derivación de cada uno está en KERNELS);
  rendimiento = operaciones útiles / mediana del tiempo de kernel en la T4 (columna gpu_kernel* de colab.csv).
Los tiempos salen de los CSV: nada escrito a mano. Las operaciones y los bytes son conteos del código (una suma, un
mínimo, una comparación o un incremento = 1 operación; una FMA = 2 FLOP); el direccionamiento no se cuenta.

Techos de la T4 (hoja de datos de NVIDIA y la cuenta de sus unidades): DRAM 320 GB/s; FP32 8,1 TFLOP/s; enteros
de 32 bits ~4,1 Tops/s (40 SM × 64 unidades INT32 × 1,59 GHz, sin FMA); FP64 0,25 TFLOP/s (1/32 de FP32).

Uso: roofline_ec2.py <docs/ec2> <salida.png>   (también imprime la tabla en Markdown)
"""
import csv
import pathlib
import statistics
import sys

# carpeta, versión, tamaño (el de la tabla), tipo, nombre, operaciones(tamaño), bytes(tamaño), derivación
KERNELS = [
    ("t1_todos_contra_todos", "gpu_kernel", 4775, "int", "T1 Floyd–Warshall por bloques",
     lambda n: 2 * n**3, lambda n: 2 * n**3 * 12288 / (2 * 32**3),
     "2n³ (una suma y un mínimo por k, i, j); por bloque de 32: 32³·2 ops sobre 3 bloques de 4 KB = 5,3 ops/B"),
    ("c1_mapa_de_calor", "gpu_kernel", 4194304, "fp32", "C1 convolución 13×13",
     lambda c: 338 * c, lambda c: 8 * c, "169 FMA = 338 FLOP por celda; leer y escribir un float = 8 B"),
    ("s2_difusion", "gpu_kernel", 4194304, "fp32", "S2 stencil (coarsening)",
     lambda c: 100 * 8 * c, lambda c: 100 * 8 * c, "8 FLOP por celda y paso, 100 pasos; 8 B por celda y paso"),
    ("s1_cobertura", "gpu_kernel_basic", 1048576, "int", "S1 stencil (básico)",
     lambda c: 100 * 8 * c, lambda c: 100 * 8 * c,
     "4 sumas y 4 mínimos por celda y barrido, 100 barridos; 8 B por celda y barrido"),
    ("p2_sentidos_unicos", "gpu_kernel", 16777216, "int", "P2 corner turning con relleno",
     lambda c: 10 * (int(c**0.5) * (int(c**0.5) - 1) // 2), lambda c: 4 * c,
     "~10 ops por par i < j (2 comparaciones, resta, 3 sumas, clave, máximo); la tabla una vez, 4 B por celda"),
    ("p1_aos_soa", "gpu_kernel", 10000000, "int", "P1 fases (SoA)",
     lambda n: 7 * n, lambda n: 16 * n, "3 restas, 3 sumas y una comparación por pedido; 4 campos de 4 B"),
    ("h1_tiempos_de_entrega", "gpu_kernel", 10000000, "int", "H1 histograma privatizado",
     lambda n: n, lambda n: 4 * n, "un incremento por entrega; 4 B por entrega"),
    ("h2_franja_restaurante", "gpu_kernel", 10000000, "int", "H2 histograma 2D intercalado",
     lambda n: 3 * n, lambda n: 8 * n, "franja (división), índice (producto y suma) e incremento; 2 × 4 B por pedido"),
    ("r2_por_restaurante", "gpu_kernel", 10000000, "int", "R2 reducción segmentada",
     lambda n: 4 * n, lambda n: 8 * n, "comparación, cuenta, suma y máximo por pedido; índice + valor = 8 B"),
    ("sc1_agrupar_por_restaurante", "gpu_kernel", 10000000, "int", "SC1 scan + reparto estable",
     lambda n: 3 * n, lambda n: 12 * n, "contar, recontar y ubicar por pedido; leer el restaurante 2 veces y escribir el índice"),
    ("r1_mas_cercanas", "gpu_kernel", 100000, "fp64", "R1 haversine + argmin (FP64)",
     lambda n: 60 * n, lambda n: 16 * n,
     "estimación: ~60 FLOP de FP64 por moto (seno, coseno, arcoseno y raíz por polinomios); latitud y longitud en double"),
]

PEAK = {"fp32": 8.1e12, "int": 4.07e12, "fp64": 0.254e12}
DRAM = 320e9


def kernel_ms(folder, version, size):
    times = []
    with open(folder / "colab.csv") as f:
        for row in csv.DictReader(f):
            if row["version"] == version and int(row["size"]) == size:
                times.append(float(row["ms"]))
    if not times:
        sys.exit(f"{folder.name}: sin filas {version} con tamaño {size}")
    return statistics.median(times)


def main(docs, out):
    points = []
    print("| kernel | tamaño | operaciones | bytes mínimos | intensidad (ops/B) | kernel (ms) | rendimiento (Gops/s) |"
          " % del techo | derivación |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
    for folder, version, size, kind, name, ops, nbytes, why in KERNELS:
        ms = kernel_ms(docs / folder, version, size)
        o, b = ops(size), nbytes(size)
        intensity, perf = o / b, o / (ms / 1000)
        roof = min(PEAK[kind], intensity * DRAM)
        points.append((name, kind, intensity, perf))
        print(f"| {name} | {size:,} | {o:.3g} | {b:.3g} | {intensity:.2f} | {ms:.4g} | {perf / 1e9:.1f} | "
              f"{100 * perf / roof:.0f} % | {why} |")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 6.5))
    xs = [10 ** (k / 20) for k in range(-30, 61)]
    colors = {"fp32": "#1f77b4", "int": "#2ca02c", "fp64": "#d62728"}
    titles = {"fp32": "FP32 8,1 TFLOP/s", "int": "INT32 ~4,1 Tops/s", "fp64": "FP64 0,25 TFLOP/s"}
    top = max(PEAK.values())
    ax.plot([x for x in xs if x * DRAM <= top], [x * DRAM / 1e9 for x in xs if x * DRAM <= top], color="gray",
            lw=1.5, label="techo de la DRAM: 320 GB/s")
    for kind, peak in PEAK.items():
        ridge = peak / DRAM  # el codo: desde acá manda el cómputo
        ax.plot([ridge, xs[-1]], [peak / 1e9, peak / 1e9], color=colors[kind], lw=1.5,
                label=f"techo {titles[kind]} (codo en {ridge:.2g} ops/B)")
    for name, kind, intensity, perf in points:
        ax.scatter(intensity, perf / 1e9, color=colors[kind], s=45, zorder=3, edgecolor="black", linewidth=0.5)
        ax.annotate(name, (intensity, perf / 1e9), textcoords="offset points", xytext=(6, -3), fontsize=8)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("intensidad aritmética (operaciones útiles por byte mínimo de DRAM)")
    ax.set_ylabel("rendimiento logrado (Gops/s o GFLOP/s)")
    ax.set_title("Roofline de la Tesla T4: el kernel principal de cada aplicación (tamaño más grande medido)")
    ax.set_xlim(0.03, 1000)
    ax.set_ylim(1, 2e4)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    print(f"\ngráfico: {out}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(pathlib.Path(sys.argv[1]), sys.argv[2])
