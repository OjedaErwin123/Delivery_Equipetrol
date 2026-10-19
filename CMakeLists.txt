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

# --- Dependencias ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenCV 4.1 REQUIRED COMPONENTS core imgproc imgcodecs highgui)

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
    target_compile_options(${target} PRIVATE -Wall -Wextra)
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
)
delivery_target_options(delivery_sim)
# SYSTEM: los warnings de los headers de OpenCV no cuentan como nuestros.
target_include_directories(delivery_sim SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(delivery_sim PRIVATE ${OpenCV_LIBS} nlohmann_json)

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

add_executable(handover_check tests/handover_check.cpp src/simulation.cpp src/thread_pool.cpp src/estimate.cpp
               src/road_graph.cpp src/courier.cpp src/travel.cpp src/restaurant.cpp src/fleet.cpp src/order_book.cpp
               src/event_log.cpp)
delivery_target_options(handover_check)
add_test(NAME handover_check COMMAND handover_check)

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
