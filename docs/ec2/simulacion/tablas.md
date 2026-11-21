# Simulación completa: tablas (generadas con tools/efecto_simulacion.py table)

Mediana [mín–máx] de 3 corridas por versión. Colab: misma T4, build con CUDA (gpu) y sin CUDA (cpu),
corridas intercaladas. Laptop: sin GPU. Sin ventana, con --frames (la foto de cada frame, sin dibujar).

## Ciudad (4775 nodos, 3000 motos)

| máquina | versión | corridas | arranque (ms reales) | asignados / s real | despacho p50 (s sim.) | despacho p90 (s sim.) | entregados / h sim. | frames / s | frame p99 (ms) | frame máx. (ms) | fallbacks | ¿consistente? |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| Intel(R) Xeon(R) CPU @ 2.00GHz x2 + Tesla T4 | gpu | 3 | 858.8 [851.8–889.1] | 22.6 [21.7–24.8] | 521.2 [507.6–574.6] | 597.2 [595.3–599.5] | 1123.0 [1083.0–1225.0] | 29.4 [29.1–29.5] | 39.0 [38.0–39.9] | 43.1 [43.1–46.5] | 0 | sí |
| Intel(R) Xeon(R) CPU @ 2.00GHz x2 + Tesla T4 | cpu | 3 | 149.9 [147.5–157.6] | 20.9 [20.2–21.2] | 523.0 [501.6–559.8] | 598.2 [595.3–598.7] | 1166.0 [1143.0–1186.0] | 29.0 [28.9–29.2] | 40.9 [39.2–45.0] | 46.6 [46.0–56.0] | 0 | sí |
| Intel(R) Core(TM) i5-6300U CPU @ 2.40GHz x4 | cpu | 3 | 427.5 [316.3–928.4] | 23.3 [20.3–24.3] | 528.0 [481.3–551.7] | 597.8 [597.0–599.4] | 1153.0 [948.0–1268.0] | 29.4 [29.4–29.7] | 51.8 [45.9–59.8] | 122.2 [121.6–124.6] | 0 | sí |

## Equipetrol (154 nodos, 8 motos)

| máquina | versión | corridas | arranque (ms reales) | asignados / s real | despacho p50 (s sim.) | despacho p90 (s sim.) | entregados / h sim. | frames / s | frame p99 (ms) | frame máx. (ms) | fallbacks | ¿consistente? |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| Intel(R) Xeon(R) CPU @ 2.00GHz x2 + Tesla T4 | gpu | 3 | 286.3 [279.9–310.8] | 2.4 [2.4–2.4] | 135.4 [135.3–143.6] | 360.2 [360.0–363.0] | 127.0 [127.0–127.0] | 30.2 [30.2–30.2] | 33.2 [33.2–33.3] | 34.7 [34.5–34.9] | 0 | sí |
| Intel(R) Xeon(R) CPU @ 2.00GHz x2 + Tesla T4 | cpu | 3 | 4.3 [4.2–4.4] | 2.4 [2.4–2.4] | 135.4 [135.3–143.6] | 360.0 [360.0–363.0] | 127.0 [127.0–127.0] | 30.2 [30.2–30.2] | 33.2 [33.2–33.3] | 34.6 [34.4–34.7] | 0 | sí |
| Intel(R) Core(TM) i5-6300U CPU @ 2.40GHz x4 | cpu | 3 | 5.9 [5.4–6.0] | 2.4 [2.4–2.4] | 144.0 [135.5–144.0] | 363.1 [360.3–363.1] | 127.0 [127.0–127.0] | 30.1 [30.1–30.1] | 33.5 [33.5–33.5] | 34.1 [33.7–34.2] | 0 | sí |

## Con la ventana abierta (laptop, una corrida)

- equipetrol: `con ventana, frames: 1805, por segundo 30.08, intervalo mediana 33.20 ms, p99 33.64 ms, máximo 33.84 ms, de más de 50 ms: 0`
- ciudad: `con ventana, frames: 689, por segundo 11.57, intervalo mediana 81.83 ms, p99 208.90 ms, máximo 246.08 ms, de más de 50 ms: 688`
