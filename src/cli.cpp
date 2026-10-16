#include "cli.hpp"

namespace {

bool isFlag(const std::string& arg) { return arg.rfind("--", 0) == 0; }

}  // namespace

CliArgs parseArgs(const std::vector<std::string>& args)
{
    if (args.empty()) {
        throw CliError("falta el archivo de configuración");
    }
    if (isFlag(args[0])) {
        throw CliError("el primer argumento debe ser el archivo de configuración, no '" + args[0] + "'");
    }
    CliArgs result;
    result.configPath = args[0];
    bool logSeen = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "--log") {
            throw CliError("argumento desconocido '" + args[i] + "'");
        }
        if (logSeen) {
            throw CliError("--log aparece más de una vez");
        }
        if (i + 1 == args.size() || isFlag(args[i + 1])) {
            throw CliError("--log necesita la ruta del archivo de log");
        }
        result.logPath = args[++i];
        logSeen = true;
    }
    return result;
}

const char* usage()
{
    return "uso: delivery_sim <config.json> [--log <path>]\n";
}
