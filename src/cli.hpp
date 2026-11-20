#pragma once

#include <stdexcept>
#include <string>
#include <vector>

// Argumentos de línea de comandos (sección 5 del enunciado, más los modos de EC2):
//   delivery_sim <config.json> [--log <path>] [--headless] [--bench <csv>] [--bench-once] [--bench-app <nombre>]
// El primero es el archivo de configuración; después, en cualquier orden y a lo sumo una vez cada una:
//   --log <path>   la ruta del log;
//   --headless     sin ventana (para una máquina sin pantalla, como Google Colab); la simulación no cambia;
//   --bench <csv>  no simula: mide las aplicaciones de GPU de EC2 (bench.hpp) y escribe el CSV;
//   --bench-once   como --bench pero con una sola repetición por tamaño (para perfilar con ncu); sin --bench
//                  no escribe CSV;
//   --bench-app <nombre>  con --bench o --bench-once, mide solo esa aplicación (p. ej. t1_apsp).
struct CliArgs {
    std::string configPath;
    std::string logPath = "events.log";
    bool headless = false;
    std::string benchPath;  // vacía = sin CSV
    bool benchOnce = false;
    std::string benchApp;   // vacía = todas
    bool bench() const { return !benchPath.empty() || benchOnce; }
};

// Argumentos que no se pueden usar. Además de la configuración, solo existen las banderas de arriba:
// cualquier otra cosa (una bandera desconocida, --log sin ruta) también es entrada inválida,
// así un error de tipeo no deja el log en otro archivo sin avisar.
class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// `args` sin el nombre del programa (argv[1] en adelante). Lanza CliError si no son válidos.
CliArgs parseArgs(const std::vector<std::string>& args);

// Cómo se usa, para imprimir en stderr junto al error.
const char* usage();
