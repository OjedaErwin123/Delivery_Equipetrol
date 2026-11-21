| kernel | tamaño | operaciones | bytes mínimos | intensidad (ops/B) | kernel (ms) | rendimiento (Gops/s) | % del techo | derivación |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| T1 Floyd–Warshall por bloques | 4,775 | 2.18e+11 | 4.08e+10 | 5.33 | 300.3 | 725.0 | 42 % | 2n³ (una suma y un mínimo por k, i, j); por bloque de 32: 32³·2 ops sobre 3 bloques de 4 KB = 5,3 ops/B |
| C1 convolución 13×13 | 4,194,304 | 1.42e+09 | 3.36e+07 | 42.25 | 3.987 | 355.6 | 4 % | 169 FMA = 338 FLOP por celda; leer y escribir un float = 8 B |
| S2 stencil (coarsening) | 4,194,304 | 3.36e+09 | 3.36e+09 | 1.00 | 15.77 | 212.8 | 67 % | 8 FLOP por celda y paso, 100 pasos; 8 B por celda y paso |
| S1 stencil (básico) | 1,048,576 | 8.39e+08 | 8.39e+08 | 1.00 | 5.53 | 151.7 | 47 % | 4 sumas y 4 mínimos por celda y barrido, 100 barridos; 8 B por celda y barrido |
| P2 corner turning con relleno | 16,777,216 | 8.39e+07 | 6.71e+07 | 1.25 | 0.6275 | 133.7 | 33 % | ~10 ops por par i < j (2 comparaciones, resta, 3 sumas, clave, máximo); la tabla una vez, 4 B por celda |
| P1 fases (SoA) | 10,000,000 | 7e+07 | 1.6e+08 | 0.44 | 0.6881 | 101.7 | 73 % | 3 restas, 3 sumas y una comparación por pedido; 4 campos de 4 B |
| H1 histograma privatizado | 10,000,000 | 1e+07 | 4e+07 | 0.25 | 0.1823 | 54.8 | 69 % | un incremento por entrega; 4 B por entrega |
| H2 histograma 2D intercalado | 10,000,000 | 3e+07 | 8e+07 | 0.38 | 0.7841 | 38.3 | 32 % | franja (división), índice (producto y suma) e incremento; 2 × 4 B por pedido |
| R2 reducción segmentada | 10,000,000 | 4e+07 | 8e+07 | 0.50 | 1.248 | 32.1 | 20 % | comparación, cuenta, suma y máximo por pedido; índice + valor = 8 B |
| SC1 scan + reparto estable | 10,000,000 | 3e+07 | 1.2e+08 | 0.25 | 15.49 | 1.9 | 2 % | contar, recontar y ubicar por pedido; leer el restaurante 2 veces y escribir el índice |
| R1 haversine + argmin (FP64) | 100,000 | 6e+06 | 1.6e+06 | 3.75 | 0.3491 | 17.2 | 7 % | estimación: ~60 FLOP de FP64 por moto (seno, coseno, arcoseno y raíz por polinomios); latitud y longitud en double |

gráfico: docs/ec2/roofline.png
