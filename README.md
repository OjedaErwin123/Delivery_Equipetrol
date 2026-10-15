# Equipetrol Delivery

Simulación concurrente en C++17 de una empresa de delivery en la Zona Equipetrol (Santa Cruz de la Sierra).

## Dependencias

| Dependencia | Versión usada | Mínimo | Cómo se obtiene |
|---|---|---|---|
| CMake | 4.2.3 (verificado también con 3.11.4) | 3.11 | Sistema |
| g++ / clang++ | 15.2 / 21.1 | g++ 7 | Sistema |
| OpenCV (`core imgproc imgcodecs highgui`) | 4.10.0 | 4.1 | Sistema (`libopencv-dev`) |
| nlohmann/json | 3.12.0 | — | **Lo descarga CMake** al configurar (`FetchContent`) |

**`cmake ..` necesita conexión a internet la primera vez:** descarga los headers de nlohmann/json
(`include.zip` del release oficial, ~300 KB) y verifica su SHA256. Queda en `build/_deps/`, así que las
siguientes configuraciones en la misma carpeta no vuelven a descargar.

## Mapa

El fondo `data/equipetrol.png` es una composición de tiles estándar de OpenStreetMap (zoom 16, 933×1014 px)
que cubre la Zona Equipetrol del Segundo al Cuarto Anillo, con la Av. San Martín. Se regenera con:

```sh
tools/osm_map.py --north -17.7538 --south -17.7745 --west -63.2060 --east -63.1860 \
                 --zoom 16 --out data/equipetrol.png
```

El script recorta en bordes de píxel enteros e imprime los bounds exactos de la imagen:
north `-17.7537957`, south `-17.7745163`, west `-63.2060194`, east `-63.1859994`.
La imagen usa proyección Web Mercator: la longitud es lineal en x, la latitud no lo es en y.

Datos del mapa © OpenStreetMap contributors, disponibles bajo la licencia ODbL
(<https://www.openstreetmap.org/copyright>).
