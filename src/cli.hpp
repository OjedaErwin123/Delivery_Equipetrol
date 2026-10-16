#pragma once

#include <stdexcept>
#include <string>
#include <vector>

// Argumentos de línea de comandos (sección 5 del enunciado):
//   delivery_sim <config.json> [--log <path>]
// El primero es el archivo de configuración; después puede venir --log con la ruta del log.
struct CliArgs {
    std::string configPath;
    std::string logPath = "events.log";
};

// Argumentos que no se pueden usar. El enunciado solo define el de la configuración y --log:
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
