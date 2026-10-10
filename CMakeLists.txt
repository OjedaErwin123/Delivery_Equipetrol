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

# --- Dependencias ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenCV 4.1 REQUIRED COMPONENTS core imgproc imgcodecs highgui)

# --- Target principal (nombre obligatorio: delivery_sim) ---
add_executable(delivery_sim
  src/main.cpp
  src/order_book.cpp
)

# SYSTEM: los warnings de los headers de OpenCV no cuentan como nuestros.
target_include_directories(delivery_sim SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(delivery_sim PRIVATE ${OpenCV_LIBS} Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(delivery_sim PRIVATE -Wall -Wextra)
  if(DELIVERY_WERROR)
    target_compile_options(delivery_sim PRIVATE -Werror)
  endif()
endif()

# --- ThreadSanitizer: mismas banderas al compilar y al enlazar ---
if(ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ENABLE_TSAN requiere GCC o Clang")
  endif()
  # Van después de las banderas del tipo de build, así que -O1 reemplaza a -O2.
  target_compile_options(delivery_sim PRIVATE
                         -fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  # En 3.11 no existe target_link_options: la bandera de enlace va aquí.
  target_link_libraries(delivery_sim PRIVATE -fsanitize=thread)
  message(STATUS "ThreadSanitizer: activado")
endif()
