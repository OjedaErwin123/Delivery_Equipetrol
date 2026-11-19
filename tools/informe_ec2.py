#!/usr/bin/env python3
"""Tablas (markdown) y gráficos de una aplicación de EC2 desde los CSV del bench (laptop y Colab).
Uso: tools/informe_ec2.py <aplicación> <laptop.csv> <colab.csv> <carpeta de salida>
Escribe <carpeta>/<aplicación>.png y <carpeta>/tablas.md, e imprime las tablas. Misma lógica que colab/ec2_gpu.ipynb:
mediana [mín–máx] de 15 repeticiones; "mejor CPU" = la versión de CPU (1 hilo o N hilos) de menor mediana en cada
tamaño; speedup = mejor CPU / GPU.
"""
import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

app, laptop_csv, colab_csv, out = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4])
laptop = pd.read_csv(laptop_csv)
colab = pd.read_csv(colab_csv)
laptop, colab = laptop[laptop.application == app], colab[colab.application == app]


def summary(df):
    g = df.groupby(["version", "threads", "size"])["ms"]
    return g.agg(median="median", min="min", max="max", runs="count").reset_index()


def best_cpu(s):
    cpu = s[s.version.isin(["cpu_1", "cpu_threads"])]
    return cpu.loc[cpu.groupby("size")["median"].idxmin()].set_index("size")


def fmt(row):
    return f"{row['median']:.4g} [{row['min']:.3g}–{row['max']:.3g}]"


def table(cpu_df, title):
    s_cpu, s_gpu = summary(cpu_df), summary(colab)
    best = best_cpu(s_cpu)
    lines = [f"**{title}**", "",
             "| tamaño | CPU 1 hilo (ms) | mejor CPU N hilos (ms) | GPU kernel (ms) | GPU punta a punta (ms) | "
             "speedup kernel | speedup punta a punta | ¿correcto? |",
             "|---:|---:|---:|---:|---:|---:|---:|:---:|"]
    for size in sorted(s_gpu["size"].unique()):
        one = s_cpu[(s_cpu.version == "cpu_1") & (s_cpu["size"] == size)].iloc[0]
        b = best.loc[size]
        kern = s_gpu[(s_gpu.version == "gpu_kernel") & (s_gpu["size"] == size)].iloc[0]
        e2e = s_gpu[(s_gpu.version == "gpu_end_to_end") & (s_gpu["size"] == size)].iloc[0]
        label = "1 hilo" if b["version"] == "cpu_1" else f"N={int(b['threads'])}"
        ok = (cpu_df[cpu_df["size"] == size].correct == "yes").all() and \
             (colab[colab["size"] == size].correct == "yes").all()
        lines.append(f"| {size} | {fmt(one)} | {fmt(b)} ({label}) | {fmt(kern)} | {fmt(e2e)} | "
                     f"{b['median'] / kern['median']:.2f}× | {b['median'] / e2e['median']:.2f}× | {'sí' if ok else 'NO'} |")
    return lines


def sweep(cpu_df, title):
    s = summary(cpu_df[cpu_df.version == "cpu_threads"])
    piv = s.pivot(index="size", columns="threads", values="median")
    lines = [f"**{title}** (mediana de N hilos contra 1 hilo de la misma versión)", "",
             "| tamaño | " + " | ".join(f"{t} hilo{'s' if t > 1 else ''}" for t in piv.columns) + " |",
             "|---:|" + "---:|" * len(piv.columns)]
    for size, row in piv.iterrows():
        lines.append(f"| {size} | " + " | ".join(f"{row[piv.columns[0]] / row[t]:.2f}×" for t in piv.columns) + " |")
    return lines


md = [f"Laptop: {laptop.machine.iloc[0]}  ", f"Colab: {colab.machine.iloc[0]}", ""]
md += table(laptop, "Veredicto: GPU (Colab, T4) contra la CPU de la laptop") + [""]
md += table(colab, "Misma máquina: GPU contra la CPU de Colab (2 vCPU)") + [""]
md += sweep(laptop, "Barrido de hilos en la laptop") + [""]
md += sweep(colab, "Barrido de hilos en Colab")
out.mkdir(parents=True, exist_ok=True)
(out / "tablas.md").write_text("\n".join(md) + "\n", encoding="utf-8")
print("\n".join(md))

# Gráfico 1: tiempo contra tamaño (log-log) con el cruce; gráfico 2: copias contra kernel.
s_lap, s_col, s_gpu = summary(laptop), summary(colab), summary(colab)
fig, ax = plt.subplots(1, 2, figsize=(13, 4.8))


def curve(axis, s, label, marker, style="-"):
    axis.errorbar(s["size"], s["median"], yerr=[s["median"] - s["min"], s["max"] - s["median"]], marker=marker,
                  ls=style, capsize=3, label=label)


curve(ax[0], s_lap[s_lap.version == "cpu_1"], "laptop: CPU 1 hilo", "o")
curve(ax[0], best_cpu(s_lap).reset_index(), "laptop: mejor CPU N hilos", "s")
curve(ax[0], best_cpu(s_col).reset_index(), "Colab: mejor CPU (2 vCPU)", "s", ":")
curve(ax[0], s_gpu[s_gpu.version == "gpu_kernel"], "T4: GPU kernel", "^")
curve(ax[0], s_gpu[s_gpu.version == "gpu_end_to_end"], "T4: GPU punta a punta", "v")
e2e = s_gpu[s_gpu.version == "gpu_end_to_end"].set_index("size")["median"]
best = best_cpu(s_lap)["median"]
wins = [n for n in e2e.index if e2e[n] < best[n]]
if wins:
    ax[0].axvline(min(wins), color="gray", ls="--", label=f"cruce contra la laptop: {min(wins)}")
ax[0].set_xscale("log"); ax[0].set_yscale("log")
ax[0].set_xlabel("tamaño"); ax[0].set_ylabel("ms (mediana; barras: mín–máx)")
ax[0].set_title(f"{app}: tiempo contra tamaño"); ax[0].grid(True, which="both", alpha=.3); ax[0].legend(fontsize=8)
kern = s_gpu[s_gpu.version == "gpu_kernel"].set_index("size")["median"]
share = pd.DataFrame({"kernels": kern / e2e, "copias y sincronización": 1 - kern / e2e})
share.plot.bar(stacked=True, ax=ax[1], color=["#4c72b0", "#dd8452"])
ax[1].set_xlabel("tamaño"); ax[1].set_ylabel("fracción del tiempo de punta a punta")
ax[1].set_title(f"{app}: copias contra kernels (T4)")
plt.tight_layout()
plt.savefig(out / f"{app}.png", dpi=120)
print("cruce contra la laptop:", min(wins) if wins else "ninguno", "| gráfico:", out / f"{app}.png")
