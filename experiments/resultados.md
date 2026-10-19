<!-- Generado por: build/delivery_experiments config/equipetrol.json experiments/raw todo -->
<!-- 2026-10-19 09:00 UTC, 4 núcleos, g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0, build RelWithDebInfo (-O2) -->

### Candidatos: todas las motos con lugar vs las k más cercanas

64 motos, un pedido cada 1,5 s simulados, 1 h simulada, semillas 1–3, pool por defecto.

| Candidatos | Rondas | Ronda media (ms) | p50 (ms) | p95 (ms) | Entregados | noCourier | Entrega media (min) |
|---|---|---|---|---|---|---|---|
| todas | 2937 | 0.815 | 0.690 | 1.857 | 812.7 | 0.0 | 13.22 |
| k = 16 | 2866 | 0.688 | 0.552 | 1.840 | 788.3 | 0.0 | 13.39 |
| k = 8 | 2925 | 0.572 | 0.460 | 1.302 | 798.0 | 0.0 | 13.29 |

### Tamaño del pool de estimaciones

El mismo escenario, estimando con todas; speedup = ronda media con 1 hilo / ronda media con n.

| Hilos | Rondas | Ronda media (ms) | p95 (ms) | Speedup | Eficiencia | Entregados |
|---|---|---|---|---|---|---|
| 1 | 2870 | 1.753 | 4.478 | 1.00 | 1.00 | 796.7 |
| 2 | 2920 | 1.234 | 2.959 | 1.42 | 0.71 | 808.3 |
| 4 | 3022 | 0.906 | 2.355 | 1.93 | 0.48 | 840.0 |
| 8 | 2985 | 0.778 | 1.893 | 2.25 | 0.28 | 818.7 |

### Averías y capacidad de la mochila

La configuración de equipetrol.json (8 motos), 1 h simulada, semillas 1–5 (promedios).

| breakdownProbability | bagCapacity | Entregados | Rechazados | Pendientes | Reasignados | Entrega media (min) |
|---|---|---|---|---|---|---|
| 0.00 | 3 | 118.0 | 23.2 | 54.6 | 0.0 | 10.66 |
| 0.02 | 3 | 93.2 | 48.6 | 54.0 | 22.4 | 13.33 |
| 0.10 | 3 | 67.0 | 76.2 | 52.6 | 49.6 | 16.41 |
| 0.02 | 1 | 68.6 | 85.4 | 41.8 | 7.8 | 11.42 |
| 0.02 | 5 | 107.2 | 28.6 | 60.0 | 28.2 | 12.17 |
