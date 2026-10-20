# Equipetrol Delivery

Simulación concurrente en C++17 de una empresa de delivery en la Zona Equipetrol (Santa Cruz de la Sierra),
dibujada en vivo sobre un mapa real de OpenStreetMap.

Datos del mapa © OpenStreetMap contributors, disponibles bajo la licencia ODbL
(<https://www.openstreetmap.org/copyright>).

## Compilar y ejecutar

```sh
mkdir build && cd build && cmake .. && make
./delivery_sim ../config/equipetrol.json            # el log va a events.log
./delivery_sim ../config/equipetrol.json --log otro.log

# ThreadSanitizer, en una carpeta aparte:
mkdir build-tsan && cd build-tsan && cmake -DENABLE_TSAN=ON .. && make
```

- La primera línea de stdout es `Simulation has started...`; la última, el resumen en JSON.
- Termina:
  - al cumplirse `durationS` (con 0, solo por señal);
  - con `SIGINT` o `SIGTERM`;
  - desde la ventana: cerrándola, con `q` o `Esc`, o con el botón **Terminar** (en el panel que abre el último
    botón de la barra, solo con el OpenCV de Qt).

  En todos los casos termina con salida 0, en menos de 5 s reales. Antes de salir, la ventana muestra el resumen
  ("Simulacion terminada"): 2 s si se pidió parar (con eso, la salida queda en ~3,3 s bajo Valgrind), 5 s si se
  cumplió `durationS`. Una tecla o la X la cierran antes.
- La ventana se puede agrandar o maximizar: el mapa se escala sin deformarse. Con el OpenCV de Qt, la barra de
  arriba tiene zoom, desplazamiento y guardar imagen.
- Sin pantalla (`DISPLAY` y `WAYLAND_DISPLAY` vacías) avisa en stderr y simula igual, sin ventana.
- Una configuración inválida o un mal uso salen con un mensaje en stderr, sin simular: código 1 para configuración o
  log inválidos, 2 para argumentos mal usados.
- `ctest` corre las pruebas de cada pieza (`tests/`).

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

OpenCV se usa **solo para la ventana**, y sin su paralelismo interno (`cv::setNumThreads(0)`): el OpenCV del sistema
paraleliza con TBB, que el proyecto no puede usar.

## Mapa

El fondo `data/equipetrol.png` es una composición de tiles estándar de OpenStreetMap (zoom 16, 933×1014 px)
que cubre la Zona Equipetrol del Segundo al Cuarto Anillo, con la Av. San Martín. Se regenera con:

```sh
tools/osm_map.py --north -17.7538 --south -17.7745 --west -63.2060 --east -63.1860 \
                 --zoom 16 --out data/equipetrol.png
```

El script recorta en bordes de píxel enteros e imprime los bounds exactos de la imagen:
north `-17.7537957`, south `-17.7745163`, west `-63.2060194`, east `-63.1859994`.
La imagen usa proyección Web Mercator: la longitud es lineal en x, la latitud no lo es en y. La ventana proyecta las
dos coordenadas linealmente ([src/projection.hpp](src/projection.hpp)). En los 0,02° de latitud que cubre la imagen,
la diferencia con Mercator es menor que 0,1 píxel.

## Configuración

`config/equipetrol.json` sigue el formato de la sección 5 del enunciado. **No se escribe a mano:** lo genera
`tools/osm_network.py` a partir de `tools/zona_equipetrol.json` (zona, restaurantes, punto de partida y
parámetros) y de las calles de OpenStreetMap (en caché en `~/.cache/osm_network.json`, así es reproducible):

```sh
tools/osm_network.py --zone tools/zona_equipetrol.json --image data/equipetrol.png \
                     --out config/equipetrol.json --preview-dir docs/red_vial
```

Contiene 154 nodos, 219 calles (151 de un solo sentido) y 7 restaurantes: Pizza Hut, Aviator, SantaMaria,
Pollos Santiago, Sicilia, Pollos Chuy y Pizza Bizarra, en orden del 4to al 2do Anillo.

## Esquema del programa

Cada caja es un hilo; cada flecha, un canal por el que dos hilos se comunican o se esperan.

```mermaid
flowchart LR
    SIG(["handler de SIGINT / SIGTERM"]) -- "atomic&lt;bool&gt; (lock-free)" --> MAIN
    MAIN["hilo principal<br/>bucle de frames, ~30 por s:<br/>mira la señal y el fin,<br/>arma la foto y dibuja la ventana"]
    GEN["generador de pedidos<br/>una llegada (ráfaga) tras otra"]
    DISP["despachador<br/>un pedido tras otro, FIFO"]
    POOL["pool de estimaciones<br/>hardware_concurrency() hilos<br/>un trabajo tras otro"]
    C["moto 1..N<br/>un pedido de su mochila tras otro"]
    BOOK[["OrderBook<br/>libro FIFO con límite"]]
    FLEET[["Fleet<br/>cupos de las mochilas"]]
    REST[["Restaurant × 7<br/>puestos + fila con tickets"]]
    LOG[["EventLog<br/>líneas + contadores en vivo"]]
    STOP[["Shutdown<br/>esperas interrumpibles"]]

    GEN -- "submit / take" --> BOOK --> DISP
    DISP -- "submit: cola monitor" --> POOL
    POOL -- "promise / future por trabajo" --> DISP
    DISP -- "assign: mochila (monitor de la moto)" --> C
    DISP <-- "tryReserve / waitForChange" --> FLEET
    C -- "release / setBroken / transferencia" --> FLEET
    C -- "transferTo (2 locks a la vez) / foodCollected" --> C
    C <-- "enter (ticket) / Slot" --> REST
    GEN & DISP & C -- "write" --> LOG
    LOG -- "stats()" --> MAIN
    C -- "view() (viaje + estado)" --> MAIN
    MAIN -- "request (detener)" --> STOP
    GEN & C -- "sleepUntil / unlessStopped" --> STOP
```

| Hilo | Cuántos | Sobre qué itera | Termina cuando |
|---|---|---|---|
| Principal ([src/simulation.cpp:520](src/simulation.cpp#L520)) | 1 | Frames de 33 ms: el fin por tiempo, la bandera de la señal, la foto para la ventana, `running` | Fin, señal, ventana cerrada o todos los hilos terminaron; después hace `join` de todos |
| Generador ([src/simulation.cpp:78](src/simulation.cpp#L78)) | 1 | Llegadas de pedidos en ráfagas, durmiendo hasta cada una | El tope de pedidos o la detención; cierra el libro |
| Despachador ([src/simulation.cpp:231](src/simulation.cpp#L231)) | 1 | Pedidos del libro, de a uno, en orden | El libro cerrado y vacío, o la detención; cierra las mochilas |
| Pool de estimaciones ([src/thread_pool.cpp](src/thread_pool.cpp)) | `hardware_concurrency()` (2 si da 0) | Trabajos de la cola | El destructor del pool: cierra, descarta lo que no empezó y hace `join` |
| Moto ([src/courier.cpp:399](src/courier.cpp#L399)) | `fleet.couriers` | El primer pedido de su mochila: viajar, recoger y entregar | Mochila cerrada y vacía, o la detención; avisa a la flota (`retire`) |
| Alcanzabilidad ([src/road_graph.cpp:56](src/road_graph.cpp#L56)) | 1 por restaurante | Un Dijkstra | Antes de empezar la simulación (`std::async` + `get`) |

El handler de señales **solo** pone una bandera `std::atomic<bool>` lock-free
([src/stop_signal.cpp:9](src/stop_signal.cpp#L9)): no toma locks ni despierta a nadie, porque eso no es seguro dentro
de un handler. La lee el bucle de frames, que de todos modos se despierta cada 33 ms para dibujar. Esa pausa por frame
es la que permite el enunciado, así que no es espera activa. Al verla, ese bucle hace todo lo demás: registra
`simulationStopping` y aborta los monitores, lo que despierta a cada hilo que esté esperando.

## Mapa de conceptos

| Carpeta | Concepto | Dónde y por qué |
|---|---|---|
| `01` | Hilos creados explícitamente, con un destino deliberado | [src/simulation.cpp:488](src/simulation.cpp#L488), [502](src/simulation.cpp#L502), [515](src/simulation.cpp#L515) y [594–597](src/simulation.cpp#L594): las motos, el despachador y el generador son `std::thread`, y `runSimulation` hace `join` de todos antes de armar el resumen. El pool hace `join` de los suyos en su destructor ([src/thread_pool.cpp:27](src/thread_pool.cpp#L27)). Nada se abandona con `detach`: un hilo suelto podría seguir escribiendo en el log después del resumen, y destruir uno sin `join` llama a `std::terminate`. |
| `01` | Una decisión que depende del hardware | [src/thread_pool.cpp:5](src/thread_pool.cpp#L5): el pool de estimaciones tiene `hardware_concurrency()` hilos, o 2 si esa pista da 0. Con 64 motos cada pedido genera 64 estimaciones: las tareas crecen con la flota pero los hilos no, porque más hilos que núcleos solo agregarían cambios de contexto (lo mide el experimento del pool). |
| `02` | Datos pasados por valor, por referencia y por movimiento | Por valor: la tarea de cada estimación copia el plan de la moto y el `shared_ptr` al mapa ([src/simulation.cpp:254](src/simulation.cpp#L254)), porque puede terminar tarde, cuando el despachador ya siguió. Por referencia: la función de traspaso captura `couriers` y la flota ([src/simulation.cpp:479](src/simulation.cpp#L479)), que viven hasta después del `join`. Por movimiento: el pedido entra a la mochila con `std::move` ([src/courier.cpp:55](src/courier.cpp#L55)). |
| `02` | Pedidos que viajan por movimiento y están en un solo lugar | `Order` borra la copia y deja el movimiento ([src/order.hpp:27–29](src/order.hpp#L27)). Pasa del generador al libro, del libro al despachador, a la mochila y, en un traspaso, a la mochila de otra moto ([src/courier.cpp:152](src/courier.cpp#L152)), siempre por `std::move`. Si se pudiera copiar, un pedido podría quedar en dos mochilas, y el verificador del enunciado lo cuenta como error. |
| `03` | Funciones miembro de objetos compartidos en hilos; contenedores de hilos | Cada moto corre `Courier::run` en su hilo ([src/simulation.cpp:488](src/simulation.cpp#L488)), y los hilos se guardan en un `std::vector<std::thread>` con `emplace_back`. `std::thread` no se puede copiar, así que solo se puede mover dentro del vector. El pool hace lo mismo con `&ThreadPool::work` ([src/thread_pool.cpp:13](src/thread_pool.cpp#L13)). |
| `03` | Propiedad compartida que garantiza la vida; ningún puntero crudo dueño | Las motos son `shared_ptr<Courier>` ([src/simulation.cpp:471](src/simulation.cpp#L471)): las comparten su hilo (copia capturada por valor) y el despachador, y el objeto vive mientras alguno lo use. El mapa es `shared_ptr<const World>`; el pool, `unique_ptr` ([src/simulation.cpp:498](src/simulation.cpp#L498)). Los únicos punteros crudos (`Slot::_restaurant`, `Carried::foodWith`) no son dueños y apuntan a objetos que viven más que ellos. |
| `04` | Tareas con un future y política de lanzamiento explícita | [src/road_graph.cpp:56](src/road_graph.cpp#L56): `std::async(std::launch::async, …)`, un Dijkstra por restaurante, en paralelo, y después los `get()`. Ahí el future que bloquea al destruirse no molesta, porque se esperan todos. Sin la política explícita, la implementación podría diferirlos hasta el `get()` y correrían uno tras otro. |
| `04` | Un promise/future como canal de un solo uso | [src/thread_pool.hpp:50–58](src/thread_pool.hpp#L50): cada trabajo del pool tiene su `std::promise`, y el despachador espera el `std::future`. A diferencia del future de `std::async`, este **no bloquea al destruirse**: una estimación tardía termina en el pool y nadie la espera, así que no congela al despachador. |
| `04` | Una espera con límite de tiempo y una excepción que cruza de hilo | [src/simulation.cpp:261–266](src/simulation.cpp#L261): `wait_until` con `quoteTimeoutMs` en tiempo real; una estimación que falla lanza `EstimateError`, que viaja por `set_exception` y sale en `get()`. El despachador la reporta en stderr y sigue con los demás candidatos. |
| `05` | Un argumento por cada objeto compartido de por qué no hay data race | La sección [Sin condiciones de carrera](#sin-condiciones-de-carrera), objeto por objeto. |
| `05` | Un tipo seguro de mover porque sus funciones especiales son correctas | `Restaurant::Slot` ([src/restaurant.hpp:33](src/restaurant.hpp#L33)): el puesto se libera en el destructor; la copia está borrada, porque dos copias liberarían el puesto dos veces; y el constructor de movimiento deja el origen en `nullptr`, así que solo el destino lo libera. `Trip` sigue la regla de cero: solo tiene vectores y valores, así que las funciones especiales que genera el compilador copian bien. |
| `06` | Exclusión mutua solo con locks RAII; secciones críticas cortas | Todos los mutex se toman con `lock_guard`, `unique_lock` o `scoped_lock`, nunca con `lock()`/`unlock()` a mano. Las secciones críticas copian y salen: la moto entrega una copia de su plan y la estimación (los Dijkstra) corre fuera del lock; el log arma la línea antes de tomar el suyo. |
| `06` | Una operación con dos locks a la vez, sin deadlock | [src/courier.cpp:118](src/courier.cpp#L118): el traspaso por avería toma los mutex de las dos motos con `std::scoped_lock`. Si A le traspasa a B mientras B le traspasa a A, cada hilo pide los mismos dos locks al revés, la receta del deadlock; `scoped_lock` los toma con el algoritmo de `std::lock`, que lo evita. El resto sigue un orden global: motos → flota → `Shutdown` → log. |
| `07` | Monitores: datos que no se tocan sin su lock | `OrderBook`, `Fleet`, `Restaurant`, `Shutdown`, `EventLog`, la cola del `ThreadPool` y la mochila de cada `Courier`: los datos son privados, y solo los tocan sus métodos, con el mutex tomado. |
| `07` | Esperas con predicado, robustas a despertares espurios y al apagado | Cada `wait` lleva predicado: la moto sin pedidos ([src/courier.cpp:96](src/courier.cpp#L96)), la fila del restaurante por ticket ([src/restaurant.cpp:13](src/restaurant.cpp#L13)), el despachador esperando lugar ([src/fleet.cpp:62](src/fleet.cpp#L62)), los hilos del pool ([src/thread_pool.cpp:38](src/thread_pool.cpp#L38)) y las esperas de tiempo simulado ([src/shutdown.hpp:67](src/shutdown.hpp#L67)). Un despertar sin novedades vuelve a dormir. Para el apagado, cada monitor tiene su `abort()`/`close()` con `notify_all`, así nadie queda dormido. |

## Sin condiciones de carrera

Por cada objeto que comparten varios hilos, la razón por la que no puede haber una condición de carrera sobre él.

| Objeto | Quiénes lo usan | Por qué no hay data race |
|---|---|---|
| `OrderBook` (libro) | generador, despachador, principal (`abort`) | Monitor: la cola y las banderas son privadas y solo se tocan con su mutex. |
| `Fleet` (cupos) | despachador, motos, principal | Monitor. "Elegir una moto con lugar y reservarle el cupo" es una sola operación con el lock tomado, así que no hay check-then-act entre hilos. |
| Mochila, viaje y estado de cada `Courier` | su hilo, el despachador (`assign`, `plan`), otra moto (traspaso), la ventana (`view`) | Monitor de la moto: `_bag`, `_trip` y las banderas solo se tocan con su mutex. El traspaso toma los dos mutex juntos. Los demás leen copias. |
| `_at`, `_rng` y estadísticas de una moto | solo su hilo | No se comparten mientras corre. Las estadísticas se leen después del `join`, que sincroniza. |
| `Restaurant` (puestos y tickets) | las motos que llegan | Monitor; el ticket se toma con el lock, y eso fija el orden de llegada. |
| `EventLog` y sus contadores en vivo | todos los hilos, la ventana | Monitor: la línea y los contadores se escriben en la misma sección crítica; `stats()` copia con el lock. |
| `Shutdown` | todos | Monitor; pedir parar y registrar `simulationStopping` son una sola operación. |
| Cola del `ThreadPool` | despachador, hilos del pool | Monitor; cada trabajo corre fuera del lock. Su resultado viaja por su `promise`, que sincroniza. |
| `World` / `RoadGraph`, `SimClock`, `SimParams` | todos | Se arman antes de lanzar los hilos y después solo se leen (`const`): sin escrituras no hay carrera. |
| Bandera de la señal | handler, hilo principal | `std::atomic<bool>` lock-free. |
| `running` (hilos que siguen) | todos los hilos, el principal | `std::atomic<int>` con `fetch_sub`, una sola operación de lectura-modificación-escritura: dos hilos que terminan a la vez no pierden un descuento. |
| Resultados del generador y del despachador | un solo hilo cada uno | Los escribe un solo hilo y se leen después de su `join`. |
| La función de traspaso de cada moto | su hilo | Se fija antes de lanzar los hilos; después solo se lee. |

ThreadSanitizer no reporta nada en las pruebas ni en `delivery_sim`; la [sección de herramientas](#análisis-con-herramientas)
lo documenta. En el build de TSan, [src/tsan_suppressions.cpp](src/tsan_suppressions.cpp) suprime **solo** los
reportes cuya pila pasa por las bibliotecas gráficas que usa OpenCV (Qt, DBus, GTK). Son carreras dentro de esas
bibliotecas, en código no instrumentado; una carrera inyectada a propósito en nuestro código se sigue reportando.

## Decisiones sobre lo que el enunciado no fija

- **Destino de un pedido:** un nodo al azar de la red, sin contar los nodos de los restaurantes. Es `unreachable` si
  no hay camino dirigido desde el restaurante.
- **Ráfagas:** cada llegada trae entre 1 y `burstMax` pedidos, con un intervalo medio de
  `meanIntervalMs × (1 + burstMax) / 2`. Así el tiempo medio entre dos pedidos sigue siendo `meanIntervalMs`.
- **`maxPending`** cuenta los pedidos dentro del libro; `unreachable` tiene prioridad sobre `queueFull`.
- **"Reproducible"** = la secuencia de pedidos (instantes, ráfagas, restaurantes, destinos). Lo que depende del
  intercalado (asignaciones, `queueFull`, averías) puede variar entre corridas.
- **Preparación:** uniforme en el rango del restaurante; empieza al asignar el pedido.
- **Restaurante:** la entrega en el puesto tarda 30 s simulados, y la moto espera la comida con el puesto tomado.
- **Mochila:** `bagCapacity` cuenta los pedidos asignados. Una visita a un restaurante recoge todos los de ese
  restaurante, y se entrega en el orden de asignación.
- **Despachador:**
  - **candidatos:** las 8 motos con lugar más cercanas al restaurante (si ninguna da una estimación usable, todas);
    era "todas" hasta que el experimento de candidatos mostró que con flotas grandes el despachador se trababa;
  - **ETA:** el recorrido completo, sin contar la fila;
  - **fallo de una estimación:** que no se pueda calcular (sin camino);
  - **`noCourier`:** cuando pasa `acceptTimeoutMs` desde la creación sin lograr asignar el pedido.
- **Averías:**
  - **viaje:** cada tramo; el instante es al azar dentro del tramo, y la moto queda en ese punto de la calle;
  - **traspaso:** siempre traspasa todo a un solo receptor, el que llega antes por calle, con prioridad sobre el
    despachador;
  - **rescate:** los pedidos ya recogidos se van a buscar al punto de la avería (15 s);
  - **reparación:** 5 min, desde que le retiraron la comida;
  - **sin otra moto que pueda trabajar:** se repara igual y sigue con sus pedidos.
- **Ventana:** cerrarla (o `q`, `Esc`, el botón Terminar) detiene la simulación; sin pantalla, se simula igual. Al
  terminar muestra el resumen 2 s (5 s si terminó por tiempo).

## Experimentos

Los corre `delivery_experiments`, un programa aparte que no forma parte de `delivery_sim`, sin ventana, desde la raíz
del repositorio:

```sh
build/delivery_experiments config/equipetrol.json experiments/raw todo
# lo mismo con el build de TSan, solo el de candidatos:
build-tsan/delivery_experiments config/equipetrol.json experiments/raw_tsan candidatos
```

- **Datos en bruto:** cada corrida de las tablas deja su `config.json` (se puede volver a correr con `delivery_sim`),
  `params.txt`, `events.log.gz` y `summary.json` en `experiments/raw/<experimento>/<variante>_seed<n>/`.
- **Condiciones comunes:** 1 hora simulada con `timeScale` 600 (6 s reales por corrida), build `RelWithDebInfo`
  (`-O2`), 4 núcleos, WSL2.
- **Qué se varía:** en cada experimento se cambia una sola cosa; todo lo demás es la configuración de
  `equipetrol.json`.
- **Tiempos:** los de ronda son **tiempo real**, desde que el despachador encola las estimaciones de un pedido hasta
  que tiene los resultados.

### 1. Candidatos: todas las motos contra las k más cercanas → **cambio de diseño**

- **Qué se cambió:** con cuántas motos estima el despachador cada pedido: todas las que tienen lugar, o las k más
  cercanas en línea recta al restaurante.
- **Qué se mantuvo:** el escenario grande (64 motos, un pedido cada 1,5 s simulados), semillas 1–3, pool por defecto
  (4 hilos).
- **Qué se midió:** el tiempo de cada ronda de estimación y la calidad (entregados, `noCourier`, entrega media).

| Candidatos | Build | Ronda media (ms) | p95 (ms) | Entregados | `queueFull` | Entrega media (min) |
|---|---|---|---|---|---|---|
| todas | normal (`-O2`) | 0,815 | 1,857 | 812,7 | 1375 | 13,22 |
| k = 16 | normal | 0,688 | 1,840 | 788,3 | 1400 | 13,39 |
| k = 8 | normal | 0,572 | 1,302 | 798,0 | 1389 | 13,29 |
| todas | ThreadSanitizer | 8,686 | 12,314 | **579,3** | **1697** | 10,57 |
| k = 16 | ThreadSanitizer | 2,880 | 4,918 | 778,0 | 1447 | 13,29 |
| k = 8 | ThreadSanitizer | 2,043 | 4,263 | **782,7** | **1406** | 13,23 |

**Conclusión:**
- Con el build normal da casi igual: k = 8 hace la ronda un 30 % más corta y entrega un 1,8 % menos, dentro de la
  diferencia entre semillas.
- Cuando estimar es caro (bajo TSan, unas 10 veces más lento, como lo correrán los evaluadores, o en un mapa de
  cientos de intersecciones), **el despachador se vuelve el cuello de botella**. Llega un pedido cada 2,5 ms reales y
  cada ronda tarda 8,7 ms: el libro se llena, suben los `queueFull` (1406 → 1697) y **se entrega un 26 % menos**.
- Con k = 8 la ronda (2,0 ms) queda por debajo del ritmo de llegada.

**Cambio:** `delivery_sim` estima con las **8 motos con lugar más cercanas** (`kNearestCandidates`,
`src/simulation.hpp`). Si ninguna de las 8 da una estimación usable (por ejemplo, porque están atrapadas), ese pedido
se vuelve a estimar con todas. **Antes / después, bajo TSan: 579 → 783 entregados, ronda 8,7 → 2,0 ms.**

### 2. Tamaño del pool de estimaciones

- **Qué se cambió:** los hilos del pool (1, 2, 4, 8).
- **Qué se mantuvo:** el mismo escenario grande, estimando con **todas** (para que haya trabajo), semillas 1–3.
- **Qué se midió:** la ronda media, el speedup (ronda con 1 hilo / ronda con n) y la eficiencia (speedup / n).

| Hilos | Ronda media (ms) | p95 (ms) | Speedup | Eficiencia | Entregados |
|---|---|---|---|---|---|
| 1 | 1,753 | 4,478 | 1,00 | 1,00 | 796,7 |
| 2 | 1,234 | 2,959 | 1,42 | 0,71 | 808,3 |
| 4 | 0,906 | 2,355 | 1,93 | 0,48 | 840,0 |
| 8 | 0,778 | 1,893 | 2,25 | 0,28 | 818,7 |

**Conclusión:** el speedup se ajusta a la **ley de Amdahl** con una parte secuencial de alrededor del 36 %.
- **La parte secuencial:** el despachador encola, espera, ordena y copia el plan de cada moto con su lock, todo en un
  solo hilo.
- **La predicción:** con 4 hilos predice 1,93 (medido 1,93) y con 8 predice 2,27 (medido 2,25).

Pasar de 4 a 8 hilos en 4 núcleos solo gana un 14 %, con la eficiencia en 0,28. **Confirma** la decisión de usar
`hardware_concurrency()` hilos (fila 01 del mapa de conceptos): más hilos que núcleos casi no ayudan.

### 3. Averías y capacidad de la mochila

- **Qué se cambió:** `breakdownProbability` (0; 0,02; 0,1) con mochila 3, y `bagCapacity` (1, 3, 5) con p = 0,02.
- **Qué se mantuvo:** `equipetrol.json` (8 motos), semillas 1–5 (promedios).

| breakdownProbability | bagCapacity | Entregados | Rechazados | `noCourier` | Reasignados | Espera hasta el traspaso (mediana / p90) | Entrega media (min) |
|---|---|---|---|---|---|---|---|
| 0 | 3 | 118,0 | 23,2 | 16,0 | 0,0 | — | 10,66 |
| 0,02 | 3 | 93,2 | 48,6 | 36,4 | 22,4 | 103 s / 426 s | 13,33 |
| 0,1 | 3 | 67,0 | 76,2 | 62,0 | 49,6 | 50 s / 300 s | 16,41 |
| 0,02 | 1 | 68,6 | 85,4 | 70,4 | 7,8 | 35 s / 119 s | 11,42 |
| 0,02 | 5 | 107,2 | 28,6 | 16,6 | 28,2 | 1 s / 283 s | 12,17 |

**Conclusión:**
- **Las averías cuestan caro:** con p = 0,02 se entrega un 21 % menos y los `noCourier` se duplican.
- **La causa no es la reparación sino el traspaso.** La averiada tiene que encontrar **una sola** moto con lugar para
  **toda** su mochila, y con mochila 3 casi todas están llenas: la espera llega a 7 minutos simulados. Mientras tanto,
  la prioridad del traspaso frena al despachador y los pedidos del libro se vencen.
- **Con mochila 5** casi siempre hay lugar enseguida y los `noCourier` vuelven al valor sin averías.

**Dos variantes probadas y descartadas, con sus números** (en `experiments/descartado/`):

| p = 0,02, mochila 3 (5 semillas) | Prioridad global (la del programa) | Sin prioridad | Receptor designado |
|---|---|---|---|
| Entregados | **93,2** | 78,6 | 88,0 |
| `noCourier` | **36,4** | 46,2 | 38,4 |

- **Sin prioridad:** los pedidos quedan atrapados en las motos averiadas, que no consiguen receptor.
- **Receptor designado:** se designa una sola moto receptora y el despachador deja de asignarle solo a ella. Depende
  de que **esa** moto se vacíe, mientras las demás se siguen llenando.

Frenar a todo el despachador deja que **varias** motos se vacíen a la vez, y el traspaso se hace con la primera que
tiene lugar. **El experimento confirmó el diseño acordado.** Repartir la mochila entre varias motos se descartó sin
implementarlo, porque el enunciado pide traspasar "a otro repartidor", en singular.

## Análisis con herramientas

La salida **completa y sin editar** de cada corrida está en [`docs/herramientas/`](docs/herramientas):
- `versiones.txt`: las versiones de cada herramienta;
- `comandos.txt`: los comandos exactos;
- por corrida: `*.stderr.txt` (la salida de la herramienta), `*.stdout.txt` (el resumen), `*.events.log.gz` (el log)
  y `*.check_log.txt` (el log verificado contra las reglas de las secciones 4, 6 y 7).

**La corrida.** `config/equipetrol.json` completa, **con la ventana abierta**: ráfagas de hasta 12 pedidos, averías
con p = 0,02 y traspasos. A los **40 s reales se le manda `SIGINT`** (`kill -INT`), así la corrida termina por señal,
a mitad de su trabajo.

**Herramientas:**
- **ThreadSanitizer**, el de g++ 15.2, con el build `-DENABLE_TSAN=ON` (`-fsanitize=thread -g -O1`);
- **Valgrind 3.26 Memcheck**, sobre el build normal, con `--leak-check=full --track-origins=yes`.

| Corrida | Hallazgos | Salida tras `SIGINT` | Log |
|---|---|---|---|
| TSan, **sin** las supresiones (antes) | **5 reportes**, todos con la pila dentro de Qt5 y libdbus (el hilo `QDBusConnection` de Qt contra el hilo principal); ninguno en nuestro código. Salida 66 (la de TSan con reportes) | 1,17 s | 24 de 24 OK |
| TSan, con las supresiones (después) | **0 reportes**, salida 0 | 1,20 s | 24 de 24 OK |
| Memcheck, con ventana | 25 errores, todos en bibliotecas (ver abajo) | 2,28 s | 24 de 24 OK |
| Memcheck, sin pantalla | **0 errores; 0 bytes perdidos** (definitely, indirectly y possibly) | 1,10 s | 24 de 24 OK |

### Hallazgos y cambios

1. **OpenCV paraleliza por dentro con TBB**, que el proyecto no puede usar. Bajo TSan, sus hilos daban reportes.
   **Cambio:** `cv::setNumThreads(0)` ([src/window.cpp](src/window.cpp)); OpenCV dibuja en el hilo que lo llama, sin
   crear hilos. Esos reportes desaparecieron sin suprimir nada.
2. **Qt (el sistema de ventanas de este OpenCV) tiene carreras propias**, entre su hilo `QDBusConnection` y el hilo
   principal: los 5 reportes de la primera fila. Probé variables de entorno para evitar ese hilo y ninguna sirvió.
   **Cambio:** [src/tsan_suppressions.cpp](src/tsan_suppressions.cpp), compilado **solo** en el build de TSan, suprime
   un reporte solo si su pila pasa por Qt, libdbus o GTK/GLib.
   - **Que no esconde lo nuestro** lo muestra [`docs/herramientas/prueba_supresiones/`](docs/herramientas/prueba_supresiones),
     un programa mínimo con ventana ([spike.cpp](docs/herramientas/prueba_supresiones/spike.cpp)):

     | Programa mínimo | Reportes |
     |---|---|
     | Sin supresiones | 6 (Qt, DBus, TBB) |
     | Con supresiones | 0 |
     | Con supresiones y una carrera inyectada a propósito en `spike.cpp` | **2, los dos en `spike.cpp`** |
3. **Un defecto nuestro, encontrado por la verificación bajo TSan** (no era una carrera sino un SEGV). Era un
   check-then-act entre el despachador y la avería:
   1. el despachador reservaba el cupo de una moto;
   2. la moto se averiaba y traspasaba toda su mochila;
   3. el despachador le entregaba el pedido igual.

   La moto averiada seguía el viaje según lo que creía tener y fallaba al entregar.
   **Cambio:** la moto revalida con su lock al recibir una asignación (si está averiada, la rechaza) y, después de una
   avería, vuelve a leer su mochila. Hay una prueba determinística (`breakdown_check`, caso 5).
   - Antes: `unreachable_check` bajo TSan, con 4 copias en paralelo, fallaba en la primera corrida.
   - Después: 0 fallas en 200.
4. **Lo que queda en Memcheck con la ventana abierta** es todo de bibliotecas:
   - 1 "Invalid read" **dentro de OpenCV** (`libopencv_highgui`, su backend de Qt) al destruir la ventana: lee un
     bloque que acaba de liberar. No destruirla lo evita, pero deja ~2,8 MB "possibly lost", así que se destruye;
   - 18 "Conditional jump … uninitialised" en Qt y en bibliotecas sin símbolos;
   - 256 bytes "definitely lost" y ~3 KB "possibly lost" en `libfontconfig`/`libexpat`, las fuentes que carga Qt.

   **Sin ventana, el programa tiene 0 errores y 0 bytes perdidos**: nuestro código está limpio.
