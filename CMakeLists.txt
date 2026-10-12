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
  src/order_book.cpp
)
delivery_target_options(delivery_sim)
# SYSTEM: los warnings de los headers de OpenCV no cuentan como nuestros.
target_include_directories(delivery_sim SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(delivery_sim PRIVATE ${OpenCV_LIBS})

# --- Pruebas: se compilan con `make` y se ejecutan con `ctest` ---
enable_testing()
add_executable(event_log_check tests/event_log_check.cpp src/event_log.cpp)
delivery_target_options(event_log_check)
add_test(NAME event_log_check COMMAND event_log_check)

add_executable(courier_check tests/courier_check.cpp src/courier.cpp src/order_book.cpp src/event_log.cpp)
delivery_target_options(courier_check)
add_test(NAME courier_check COMMAND courier_check)

if(ENABLE_TSAN)
  message(STATUS "ThreadSanitizer: activado")
endif()
