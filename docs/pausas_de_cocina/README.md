# Pausas de cocina (variante B)

Los restaurantes cierran la ventanilla de entrega unos minutos cada cierto tiempo. Durante una **pausa** la cocina
sigue preparando, pero **ninguna moto empieza a ocupar un puesto**: las que ya lo tenían terminan, y las que llegan se
suman a la fila y esperan. Cuando la pausa termina, la fila se atiende en el mismo orden de llegada de siempre.

## Cómo se usa

```sh
mkdir build && cd build && cmake .. && make
./delivery_sim ../config/equipetrol_pausas.json
python3 ../tools/verificar_orden_llegada.py events.log salida.txt   # con: ./delivery_sim ... > salida.txt
```

La configuración gana un bloque **opcional**:

```json
"kitchen": { "pauseEveryMs": 900000, "pauseMs": 240000 }
```

- Son enteros, en tiempo simulado, con `pauseEveryMs > 0` y `0 ≤ pauseMs < pauseEveryMs`. Fuera de rango es entrada
  inválida: mensaje, salida 1 y no se simula.
- **Sin el bloque no hay pausas,** y la salida es exactamente la de siempre.
- **`pauseMs = 0` tampoco tiene pausas:** el intervalo `[s, s)` no contiene ningún instante.

`config/equipetrol_pausas.json` es el escenario de prueba de la variante: `config/equipetrol.json` con ese bloque y
todos los `pickupSlots` en 1. Va en un archivo aparte para que `equipetrol.json`, y con él la salida de DP.11 y los
experimentos, no cambien.

El restaurante número `i` (en el orden del JSON) de un total de `R` está en pausa en
`[k·P + i·P/R, k·P + i·P/R + D)`, con k = 0, 1, 2, … y división entera. Las pausas quedan escalonadas y, con
D > P/R, se superponen entre restaurantes.

## Log y resumen

Hay dos eventos nuevos, con `t` y `restaurant`. Se registran exactamente en los límites del calendario y nunca después
de `simulationStopping`:

```json
{"t": 128571, "event": "restaurantPaused", "restaurant": "r1"}
{"t": 368571, "event": "restaurantResumed", "restaurant": "r1"}
```

El resumen agrega, **solo si la configuración trae `kitchen`**:

- **`"pauses"`:** igual al número de líneas `restaurantPaused`.
- **`"pickupsDelayedByPause"`:** las veces que una moto ya estaba en la fila cuando empezó una pausa, o entró a la fila
  durante una pausa. Se cuentan las visitas que llegaron a tomar el puesto, porque eso es lo que se puede recalcular
  desde el log: cada `pickupStarted` trae `arrivedAt` y `t`. Si la detención saca a una moto de la fila, ese retiro no
  ocurrió y no cuenta.

## El diseño

| Pieza | Dónde | Por qué ahí |
|---|---|---|
| El calendario | `PauseSchedule` en `src/restaurant.hpp` | Es **la única** cuenta de las pausas. La usan el restaurante (el mecanismo) y el hilo del calendario (los eventos), así que el log y el mecanismo no pueden contradecirse. Es inmutable y se lee sin lock |
| “No se toma un puesto en pausa” | `Restaurant::enter`, el monitor que ya tenía los puestos y los tickets | Es una condición más para tomar un puesto, y tiene que evaluarse con el mismo lock que el turno y `_inUse` |
| Los eventos de pausa | un hilo nuevo, `markPauses`, en `src/simulation.cpp` | Tienen que escribirse aunque no haya ninguna moto en el restaurante, y ningún hilo existente despierta en esos instantes |
| `pickupsDelayedByPause` | `Restaurant::enter`, al tomar el puesto | Ahí se conocen `arrivedAt` y `startedAt` de la visita, en una sola sección crítica: no hay doble conteo |
| Configuración y resumen | `src/config.*`, `Summary`, `toJson` | Donde ya se validaba y se imprimía todo lo demás |

### El mecanismo: calendario calculado y espera con plazo

```cpp
for (;;) {
    if (_aborted) return std::nullopt;                       // detención
    const std::int64_t now = clock.nowMs();
    const bool paused = _pauses.pausedAt(now);
    if (!paused && ticket == _nowServing && _inUse < _slots) break;
    if (paused) _turn.wait_until(lock, clock.realTimeAt(_pauses.pauseEnd(now) + 1));   // CON plazo
    else        _turn.wait(lock);                                                     // la despierta otra moto
}
```

- **El orden de llegada se conserva:** el ticket se saca al llegar y la pausa no lo toca. `pickupSlots` también, porque
  `_inUse < _slots` sigue en la condición.
- **La espera tiene plazo porque el fin de la pausa no lo avisa nadie:** lo trae el paso del tiempo. Una moto que espera
  su turno o un puesto sigue con `wait`, porque eso sí lo cambia otra moto. En cada vuelta la moto duerme: no hay espera
  activa.
- **Es exacto por construcción:** la pausa se pregunta con el lock tomado y con el mismo reloj con que se leen
  `arrivedAt` y `startedAt`, así que ningún `startedAt` cae dentro de una pausa.
- **El `+ 1`:** `nowMs()` trunca. Al despertar justo en el fin, la cuenta podría dar `fin − 1`, la moto seguiría “en
  pausa” y volvería enseguida con el plazo ya vencido, girando.
- **Detención:** `abort()` hace `notify_all` y despierta también a las motos que esperan con plazo.

**Descartado: una bandera `_paused` que prende y apaga un hilo.** El hilo despierta unos milisegundos reales tarde, y con
`timeScale` 60 cada milisegundo real son 60 ms simulados. En ese hueco una moto podría tomar un puesto dentro de la
pausa: un check-then-act con el tiempo.

### El hilo del calendario

- **Un solo hilo para todos los restaurantes,** con una cola de los próximos límites ordenada por tiempo. Duerme con
  `Shutdown::sleepUntil` hasta el más próximo, lo registra y agrega el siguiente de ese restaurante.
- **Escribe el `t` del calendario,** no el de su despertar, igual que el generador con `orderCreated`.
- **Por qué nunca queda un evento después de `simulationStopping`:** escribe con `Shutdown::unlessStopped`, que corre
  con el mismo lock que `request()`, la función que escribe `simulationStopping`. El evento se escribe antes de la orden
  de parar o se niega. Además, se niega todo `t ≥ fin`: si una pausa termina después del fin, no hay
  `restaurantResumed`.
- **Locks:** mientras duerme, no tiene ninguno. Al escribir toma Shutdown → log, el orden que ya existía. No toma el
  lock del restaurante: no se agrega ninguna arista al orden de locks.
- **Destino:**
  - no cuenta en `running`: no es trabajo pendiente y no termina solo;
  - termina con la orden de parar y se une (`join`) con los demás;
  - en el fin natural (`running` llega a 0, con tope de pedidos), el hilo principal llama a `shutdown.request` con una
    **acción vacía**, sin `simulationStopping`. Sin eso, el `join` se colgaría.

**Descartado:**
- **un hilo por restaurante:** crece con R, y el hilo solo escribe líneas, así que la independencia no gana nada;
- **el bucle de frames:** 33 ms reales son ~2 s simulados, no es exacto;
- **una bandera de parada propia:** duplicaría `Shutdown`.

## Cómo se probó

WSL2 Ubuntu, g++ 15.2, Valgrind 3.26.

**Pruebas nuevas** (`ctest`: 33 de 33, 0 warnings):

| Prueba | Qué comprueba | Si se rompe |
|---|---|---|
| `config_check` | 6 casos inválidos del bloque `kitchen`, y su lectura | — |
| `restaurant_check` | el calendario en los bordes; una pausa con fila: nadie entra en la pausa, después en orden de llegada, 1 puesto, 4 demoras; `abort` en mitad de una pausa | sin la condición de pausa, **falla**; sin el plazo, **se cuelga** |
| `pause_check` | simulaciones completas: eventos en los límites exactos, alternancia, nada después de `simulationStopping`, ningún `pickupStarted` en pausa, los dos contadores iguales al log, sin `kitchen` el resumen no cambia, el fin natural, la parada en mitad de una pausa | sin la acción vacía, **se cuelga**; con la hora del despertar, **falla** |

```
pausa: llegadas/puestos (s simulados): 0/0 20/1100 401/1150 431/1201 462/1251; demoras = 4
por tiempo: 20 restaurantPaused, 20 restaurantResumed, 73 pickupStarted; pauses 20, pickupsDelayedByPause 16 (desde el log: 16)
fin natural: 15 creados, 15 entregados, en 204 ms
parada en pausa: terminó 29 ms después de la orden; último evento simulationStopping
```

**El escenario de la variante** (`config/equipetrol_pausas.json`, una hora simulada, sin ventana):

| Corrida | Salida | Validador | Herramienta |
|---|---|---|---|
| Normal | 0, 60 s, CPU 1 % | OK: 28 pausas, 21 demoras, iguales al resumen | — |
| ThreadSanitizer | 0 | OK | **0 reportes** |
| Memcheck | 0 | OK | **0 errores, 0 bytes perdidos** |
| `kill -INT` a los 3 s reales (180 000 ms simulados, con r0 y r1 en pausa) | 0 | — | sale en 0,04 s; bajo TSan, 0,05 s; con `SIGTERM`, 0,08 s |

Las 28 pausas son 7 restaurantes × 4, una cada 900 s en 3600 s. Con la ventana, la salida tras la señal suma los 2 s de
la pantalla final.

**En el log**, en el restaurante r1, dos motos llegan durante la pausa y toman el puesto después del
`restaurantResumed`, en orden de llegada:

```json
{"t": 128571, "event": "restaurantPaused", "restaurant": "r1"}
{"t": 368571, "event": "restaurantResumed", "restaurant": "r1"}
{"t": 368581, "event": "pickupStarted", "order": "o3", "courier": "c2", "restaurant": "r1", "arrivedAt": 189179}
{"t": 398594, "event": "pickupStarted", "order": "o6", "courier": "c4", "restaurant": "r1", "arrivedAt": 189240}
```

c4 empieza 30 s después de c2: es la entrega en el puesto de c2, con un solo puesto.

**El validador** (`tools/verificar_orden_llegada.py`) sigue aceptando las corridas sin pausas y ahora comprueba cuatro
cosas más:
- que pausa y reanudación alternan por restaurante, y ninguna queda después de `simulationStopping`;
- que ningún `pickupStarted` cae dentro de una pausa;
- que `pauses` del resumen es igual a las líneas `restaurantPaused`;
- que `pickupsDelayedByPause`, recalculado desde el log, es igual al del resumen.

## Limitaciones

- La ETA del despachador no considera la pausa. Ya no consideraba la fila del restaurante, así que sigue siendo una cota
  inferior en ese tramo.
- La ventana no dibuja qué restaurante está en pausa: la variante no lo pide.

## Conceptos del curso

- **07:** monitor con una condición más; espera con predicado y con plazo (`wait_until`); `notify_all`, porque el que
  sigue es una moto concreta; `abort()` despierta también a los que esperan con plazo.
- **01:** un hilo nuevo con un destino definido (`join`), y una cantidad fija de hilos, que no crece con los
  restaurantes.
- **05 y 06:** el calendario es inmutable y se lee sin lock; el contador cambia dentro de la sección crítica del
  restaurante; los eventos usan `unlessStopped`, que comprueba y actúa en una sola operación.
