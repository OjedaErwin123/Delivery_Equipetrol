#pragma once

#include <cstddef>
#include <vector>

#include "thread_pool.hpp"

// EC2, C1 (convolución): el mapa de calor de la demanda. Una grilla de width x height celdas sobre la ciudad con la
// cantidad de pedidos que salen de cada celda (el origen de un pedido es su restaurante), suavizada con un filtro
// gaussiano de (2·kRadius + 1)² coeficientes. Fuera de la grilla cuenta como 0 (los pedidos no salen de afuera de
// la ciudad). Flotantes: la CPU y la GPU suman los mismos productos en el mismo orden, pero la GPU usa FMA
// (multiplicar y sumar con un solo redondeo): coinciden dentro de 1e-5 del valor máximo de la grilla
// (heatmap_check y gpu_heatmap_check lo comprueban).
constexpr int kRadius = 6;                     // 13 x 13 = 169 coeficientes
constexpr int kTaps = 2 * kRadius + 1;
constexpr float kSigmaCells = 2.0f;

struct Grid {
    int width = 0, height = 0;
    std::vector<float> cells;  // fila por fila
    float at(int x, int y) const { return cells[static_cast<std::size_t>(y) * width + x]; }
};

// Los kTaps x kTaps coeficientes del gaussiano, normalizados para que sumen 1 (el total de pedidos se conserva
// salvo lo que el filtro "derrama" afuera de la grilla en los bordes).
std::vector<float> gaussianFilter();

// CPU, 1 hilo: la convolución directa, celda por celda.
Grid convolve(const Grid& in, const std::vector<float>& filter);

// CPU, N hilos: las filas de salida se reparten en tramos entre los trabajos del pool (strips por filas, carpeta
// 10); cada trabajo escribe solo sus filas.
Grid convolveParallel(const Grid& in, const std::vector<float>& filter, ThreadPool& pool);

// La mayor diferencia absoluta entre dos grillas del mismo tamaño, y el mayor valor absoluto de `a`.
float maxDifference(const Grid& a, const Grid& b);
float maxValue(const Grid& a);
