# Mínimo 3.11 (sección 8 del enunciado): no usar nada posterior
# (p. ej. target_link_options es 3.13, add_compile_definitions es 3.12).
cmake_minimum_required(VERSION 3.11)
project(EquipetrolDelivery LANGUAGES CXX)

# --- Estándar: C++17 puro, sin extensiones GNU (-std=c++17, no gnu++17) ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Tipo de build por defecto: los evaluadores corren `cmake ..` sin argumentos ---
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Tipo de build" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

option(ENABLE_TSAN "Instrumentar delivery_sim con ThreadSanitizer" OFF)
option(DELIVERY_WERROR "Tratar los warnings como errores (solo uso local)" OFF)

if(ENABLE_TSAN AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "ENABLE_TSAN requiere GCC o Clang")
endif()

# --- GPU (EC2): apagada por defecto ---
# Sin USE_CUDA, todo compila con el CMake 3.11 de la sección 8 y los .cu se compilan como C++ (ver
# delivery_cuda_sources). Con USE_CUDA=ON los compila nvcc; eso pide CMake 3.18 (CMAKE_CUDA_ARCHITECTURES y
# CUDA 17), que solo se usa en la máquina con GPU (Google Colab). Arquitectura por defecto: sm_75, la T4.
option(USE_CUDA "Compilar los kernels con nvcc para una GPU NVIDIA real (pide CMake >= 3.18)" OFF)
if(USE_CUDA)
  if(CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "USE_CUDA=ON pide CMake 3.18 o posterior (sin CUDA alcanza 3.11)")
  endif()
  if(ENABLE_TSAN)
    message(FATAL_ERROR "USE_CUDA y ENABLE_TSAN no se combinan: TSan se corre sobre el build sin CUDA")
  endif()
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 75)
  endif()
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_EXTENSIONS OFF)
endif()

# --- Dependencias ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# Ventana (OpenCV): encendida por defecto, como pide la sección 8. Apagada, delivery_sim se compila sin
# OpenCV y corre sin ventana, igual que cuando no hay pantalla. Sirve en la máquina con GPU (Google Colab),
# donde no hay pantalla e instalar libopencv-dev tarda más de 30 minutos.
option(DELIVERY_WINDOW "Compilar la ventana con OpenCV" ON)
if(DELIVERY_WINDOW)
  find_package(OpenCV 4.1 REQUIRED COMPONENTS core imgproc imgcodecs highgui)
endif()

# JSON: nlohmann/json 3.12.0 (MIT, solo headers). FetchContent lo descarga al configurar
# (necesita internet en `cmake ..`): solo los headers del release, verificados por hash.
# FetchContent existe desde CMake 3.11; FetchContent_MakeAvailable es de 3.14, por eso
# se usa FetchContent_Populate.
include(FetchContent)
# Las políticas solo existen en CMake nuevos (en 3.11 estos if no hacen nada):
#  - CMP0135 (3.24): los archivos extraídos llevan la fecha de extracción.
#  - CMP0169 (3.30): permite seguir llamando FetchContent_Populate(nombre), que es la
#    forma que existe en 3.11 (los CMake nuevos la marcan como obsoleta).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip
  URL_HASH SHA256=b8cb0ef2dd7f57f18933997c9934bb1fa962594f701cd5a8d3c2c80541559372
)
FetchContent_GetProperties(nlohmann_json)
if(NOT nlohmann_json_POPULATED)
  FetchContent_Populate(nlohmann_json)
endif()
add_library(nlohmann_json INTERFACE)
# SYSTEM: los warnings de la biblioteca no cuentan como nuestros.
target_include_directories(nlohmann_json SYSTEM INTERFACE ${nlohmann_json_SOURCE_DIR}/single_include)

# Opciones comunes a todos los ejecutables del proyecto (el programa y las pruebas):
# hilos, warnings y, si se pide, ThreadSanitizer al compilar y al enlazar.
function(delivery_target_options target)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Solo a los archivos de C++: nvcc no entiende -Wall; a los .cu se los pasa al compilador del host.
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
    if(USE_CUDA)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)
    endif()
    if(DELIVERY_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(ENABLE_TSAN)
    # Van después de las banderas del tipo de build, así que -O1 reemplaza a -O2.
    target_compile_options(${target} PRIVATE -fsanitize=thread -g -O1 -fno-omit-frame-pointer)
    # En 3.11 no existe target_link_options: la bandera de enlace va aquí.
    target_link_libraries(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

# --- Target principal (nombre obligatorio: delivery_sim) ---
add_executable(delivery_sim
  src/main.cpp
  src/cli.cpp
  src/config.cpp
  src/simulation.cpp
  src/thread_pool.cpp
  src/estimate.cpp
  src/road_graph.cpp
  src/stop_signal.cpp
  src/courier.cpp
  src/travel.cpp
  src/restaurant.cpp
  src/fleet.cpp
  src/order_book.cpp
  src/event_log.cpp
  src/nearest.cpp
  src/apsp.cpp
  src/histogram.cpp
  src/grouping.cpp
  src/segment_stats.cpp
  src/report.cpp
  src/bench.cpp
)
delivery_target_options(delivery_sim)
target_link_libraries(delivery_sim PRIVATE nlohmann_json)
if(USE_CUDA)
  # EC2: el hilo dueño de la GPU y los kernels. Sin USE_CUDA, delivery_sim no tiene nada de GPU (ni la emulación).
  target_sources(delivery_sim PRIVATE src/gpu/nearest_gpu.cu src/gpu/apsp_gpu.cu src/gpu/histogram_gpu.cu
                 src/gpu/grouping_gpu.cu src/gpu/segment_stats_gpu.cu src/gpu/gpu_owner.cpp)
  target_compile_definitions(delivery_sim PRIVATE DELIVERY_USE_CUDA)
endif()
if(DELIVERY_WINDOW)
  target_sources(delivery_sim PRIVATE src/window.cpp)
  if(ENABLE_TSAN)
    # Supresiones de TSan para las bibliotecas gráficas que usa OpenCV (ver el archivo).
    target_sources(delivery_sim PRIVATE src/tsan_suppressions.cpp)
  endif()
  # SYSTEM: los warnings de los headers de OpenCV no cuentan como nuestros.
  target_include_directories(delivery_sim SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(delivery_sim PRIVATE ${OpenCV_LIBS})
else()
  target_sources(delivery_sim PRIVATE src/window_off.cpp)
endif()

# --- Pruebas: se compilan con `make` y se ejecutan con `ctest` ---
enable_testing()
add_executable(event_log_check tests/event_log_check.cpp src/event_log.cpp)
delivery_target_options(event_log_check)
add_test(NAME event_log_check COMMAND event_log_check)

add_executable(courier_check tests/courier_check.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/road_graph.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(courier_check)
add_test(NAME courier_check COMMAND courier_check)

add_executable(order_book_check tests/order_book_check.cpp src/order_book.cpp)
delivery_target_options(order_book_check)
add_test(NAME order_book_check COMMAND order_book_check)

add_executable(flow_check tests/flow_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(flow_check)
add_test(NAME flow_check COMMAND flow_check)

add_executable(json_check tests/json_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(json_check)
target_link_libraries(json_check PRIVATE nlohmann_json)
add_test(NAME json_check COMMAND json_check)

add_executable(orders_check tests/orders_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(orders_check)
add_test(NAME orders_check COMMAND orders_check)

add_executable(stop_check tests/stop_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(stop_check)
add_test(NAME stop_check COMMAND stop_check)

add_executable(signal_check tests/signal_check.cpp src/stop_signal.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(signal_check)
add_test(NAME signal_check_int COMMAND signal_check INT)
add_test(NAME signal_check_term COMMAND signal_check TERM)

add_executable(travel_check tests/travel_check.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/road_graph.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(travel_check)
add_test(NAME travel_check COMMAND travel_check)

add_executable(pickup_check tests/pickup_check.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/road_graph.cpp
               src/order_book.cpp src/event_log.cpp)
delivery_target_options(pickup_check)
add_test(NAME pickup_check COMMAND pickup_check)

add_executable(fleet_check tests/fleet_check.cpp src/fleet.cpp src/order_book.cpp)
delivery_target_options(fleet_check)
add_test(NAME fleet_check COMMAND fleet_check)

add_executable(bag_check tests/bag_check.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp
               src/road_graph.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(bag_check)
add_test(NAME bag_check COMMAND bag_check)

add_executable(pool_check tests/pool_check.cpp src/thread_pool.cpp)
delivery_target_options(pool_check)
add_test(NAME pool_check COMMAND pool_check)

add_executable(estimate_check tests/estimate_check.cpp src/estimate.cpp src/courier.cpp src/travel.cpp
               src/restaurant.cpp src/fleet.cpp src/road_graph.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(estimate_check)
add_test(NAME estimate_check COMMAND estimate_check)

add_executable(breakdown_check tests/breakdown_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(breakdown_check)
add_test(NAME breakdown_check COMMAND breakdown_check)

# Experimentos (DP.10): no es una prueba; se corre a mano (ver README).
add_executable(delivery_experiments tools/experiments.cpp src/config.cpp src/simulation.cpp src/thread_pool.cpp
               src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp
               src/order_book.cpp src/event_log.cpp)
delivery_target_options(delivery_experiments)
target_link_libraries(delivery_experiments PRIVATE nlohmann_json)

add_executable(projection_check tests/projection_check.cpp)
delivery_target_options(projection_check)
add_test(NAME projection_check COMMAND projection_check)

add_executable(frame_check tests/frame_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(frame_check)
add_test(NAME frame_check COMMAND frame_check)

add_executable(rescue_check tests/rescue_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(rescue_check)
add_test(NAME rescue_check COMMAND rescue_check)

add_executable(handover_check tests/handover_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(handover_check)
add_test(NAME handover_check COMMAND handover_check)

add_executable(pause_check tests/pause_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(pause_check)
add_test(NAME pause_check COMMAND pause_check)

add_executable(transfer_check tests/transfer_check.cpp src/courier.cpp src/estimate.cpp src/travel.cpp
               src/restaurant.cpp src/fleet.cpp src/road_graph.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(transfer_check)
add_test(NAME transfer_check COMMAND transfer_check)

add_executable(place_check tests/place_check.cpp src/estimate.cpp src/travel.cpp src/road_graph.cpp)
delivery_target_options(place_check)
add_test(NAME place_check COMMAND place_check)

add_executable(dispatch_check tests/dispatch_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(dispatch_check)
add_test(NAME dispatch_check COMMAND dispatch_check)

add_executable(restaurant_check tests/restaurant_check.cpp src/restaurant.cpp)
delivery_target_options(restaurant_check)
add_test(NAME restaurant_check COMMAND restaurant_check)

add_executable(config_check tests/config_check.cpp src/config.cpp)
delivery_target_options(config_check)
target_link_libraries(config_check PRIVATE nlohmann_json)
add_test(NAME config_check COMMAND config_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

add_executable(road_graph_check tests/road_graph_check.cpp src/road_graph.cpp src/config.cpp)
delivery_target_options(road_graph_check)
target_link_libraries(road_graph_check PRIVATE nlohmann_json)
add_test(NAME road_graph_check COMMAND road_graph_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

add_executable(cli_check tests/cli_check.cpp src/cli.cpp src/config.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp
               src/order_book.cpp src/event_log.cpp)
delivery_target_options(cli_check)
target_link_libraries(cli_check PRIVATE nlohmann_json)
add_test(NAME cli_check COMMAND cli_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

add_executable(unreachable_check tests/unreachable_check.cpp src/config.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp
               src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(unreachable_check)
target_link_libraries(unreachable_check PRIVATE nlohmann_json)
add_test(NAME unreachable_check COMMAND unreachable_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

# delivery_sim con entrada inválida: debe salir con código distinto de cero (WILL_FAIL) sin simular.
add_test(NAME delivery_sim_sin_argumentos COMMAND delivery_sim)
add_test(NAME delivery_sim_archivo_inexistente COMMAND delivery_sim no-existe.json)
add_test(NAME delivery_sim_bandera_desconocida COMMAND delivery_sim ${CMAKE_SOURCE_DIR}/config/equipetrol.json --lgo x.log)
add_test(NAME delivery_sim_log_sin_ruta COMMAND delivery_sim ${CMAKE_SOURCE_DIR}/config/equipetrol.json --log)
set_tests_properties(delivery_sim_sin_argumentos delivery_sim_archivo_inexistente delivery_sim_bandera_desconocida
                     delivery_sim_log_sin_ruta PROPERTIES WILL_FAIL TRUE)

if(ENABLE_TSAN)
  message(STATUS "ThreadSanitizer: activado")
endif()

# --- GPU (EC2): kernels y sus pruebas de igualdad ---
# Con USE_CUDA=ON los .cu los compila nvcc. Sin CUDA, el compilador de C++ los compila como C++ (-x c++) y
# src/gpu/cuda_compat.h emula la GPU con hilos: sirve para las pruebas gpu_*_check (el kernel da lo mismo que
# la CPU), nunca para medir. COMPILE_OPTIONS de un archivo existe desde CMake 3.11.
function(delivery_cuda_sources)
  if(NOT USE_CUDA)
    set_source_files_properties(${ARGN} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;c++")
  endif()
endfunction()

delivery_cuda_sources(tests/gpu_smoke_check.cu tests/gpu_nearest_check.cu tests/gpu_apsp_check.cu
                      tests/gpu_histogram_check.cu tests/gpu_grouping_check.cu tests/gpu_segment_stats_check.cu
                      src/gpu/nearest_gpu.cu src/gpu/apsp_gpu.cu src/gpu/histogram_gpu.cu src/gpu/grouping_gpu.cu
                      src/gpu/segment_stats_gpu.cu)
# Lo que usa el hilo dueño de la GPU (gpu_owner.cpp): todas las aplicaciones.
set(DELIVERY_GPU_SOURCES src/gpu/nearest_gpu.cu src/gpu/apsp_gpu.cu src/gpu/histogram_gpu.cu src/gpu/grouping_gpu.cu
    src/gpu/segment_stats_gpu.cu src/gpu/gpu_owner.cpp src/nearest.cpp src/apsp.cpp src/histogram.cpp
    src/grouping.cpp src/segment_stats.cpp src/road_graph.cpp src/thread_pool.cpp)
add_executable(gpu_smoke_check tests/gpu_smoke_check.cu)
delivery_target_options(gpu_smoke_check)
add_test(NAME gpu_smoke_check COMMAND gpu_smoke_check)

# R1: las motos más cercanas (reducción). Versiones de CPU y prueba de igualdad de la GPU.
add_executable(nearest_check tests/nearest_check.cpp src/nearest.cpp src/road_graph.cpp src/thread_pool.cpp)
delivery_target_options(nearest_check)
add_test(NAME nearest_check COMMAND nearest_check)

add_executable(gpu_nearest_check tests/gpu_nearest_check.cu ${DELIVERY_GPU_SOURCES})
delivery_target_options(gpu_nearest_check)
add_test(NAME gpu_nearest_check COMMAND gpu_nearest_check)

# T1: la tabla de todos contra todos (Floyd–Warshall por bloques). Versiones de CPU y prueba de igualdad de la GPU.
add_executable(apsp_check tests/apsp_check.cpp src/apsp.cpp src/road_graph.cpp src/thread_pool.cpp src/config.cpp)
delivery_target_options(apsp_check)
target_link_libraries(apsp_check PRIVATE nlohmann_json)
add_test(NAME apsp_check COMMAND apsp_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

add_executable(gpu_apsp_check tests/gpu_apsp_check.cu ${DELIVERY_GPU_SOURCES} src/config.cpp)
delivery_target_options(gpu_apsp_check)
target_link_libraries(gpu_apsp_check PRIVATE nlohmann_json)
add_test(NAME gpu_apsp_check COMMAND gpu_apsp_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

# H1: el histograma de los tiempos de entrega. Versiones de CPU, prueba de igualdad de la GPU y el reporte
# (--report: la simulación, su log releído y el histograma).
add_executable(histogram_check tests/histogram_check.cpp src/histogram.cpp src/thread_pool.cpp)
delivery_target_options(histogram_check)
add_test(NAME histogram_check COMMAND histogram_check)

add_executable(gpu_histogram_check tests/gpu_histogram_check.cu ${DELIVERY_GPU_SOURCES})
delivery_target_options(gpu_histogram_check)
add_test(NAME gpu_histogram_check COMMAND gpu_histogram_check)

add_executable(report_check tests/report_check.cpp src/report.cpp src/histogram.cpp src/grouping.cpp
               src/segment_stats.cpp src/config.cpp
               src/simulation.cpp src/thread_pool.cpp src/estimate.cpp src/road_graph.cpp src/courier.cpp
               src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(report_check)
target_link_libraries(report_check PRIVATE nlohmann_json)
add_test(NAME report_check COMMAND report_check ${CMAKE_SOURCE_DIR}/config/equipetrol.json)

# SC1: los pedidos agrupados por restaurante (counting sort estable con scan). Versiones de CPU y prueba de
# igualdad de la GPU; el reporte lo prueba report_check.
add_executable(grouping_check tests/grouping_check.cpp src/grouping.cpp src/thread_pool.cpp)
delivery_target_options(grouping_check)
add_test(NAME grouping_check COMMAND grouping_check)

add_executable(gpu_grouping_check tests/gpu_grouping_check.cu ${DELIVERY_GPU_SOURCES})
delivery_target_options(gpu_grouping_check)
add_test(NAME gpu_grouping_check COMMAND gpu_grouping_check)

# R2: estadísticas por restaurante (reducción segmentada sobre los grupos de SC1). Versiones de CPU y prueba de
# igualdad de la GPU; el reporte lo prueba report_check.
add_executable(segment_stats_check tests/segment_stats_check.cpp src/segment_stats.cpp src/grouping.cpp
               src/thread_pool.cpp)
delivery_target_options(segment_stats_check)
add_test(NAME segment_stats_check COMMAND segment_stats_check)

add_executable(gpu_segment_stats_check tests/gpu_segment_stats_check.cu ${DELIVERY_GPU_SOURCES})
delivery_target_options(gpu_segment_stats_check)
add_test(NAME gpu_segment_stats_check COMMAND gpu_segment_stats_check)
