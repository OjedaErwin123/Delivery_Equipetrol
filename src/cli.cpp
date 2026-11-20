#include "cli.hpp"

#include <set>

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
    std::set<std::string> seen;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (flag != "--log" && flag != "--headless" && flag != "--bench" && flag != "--bench-once" &&
            flag != "--bench-app") {
            throw CliError("argumento desconocido '" + flag + "'");
        }
        if (!seen.insert(flag).second) {
            throw CliError(flag + " aparece más de una vez");
        }
        if (flag == "--headless") {
            result.headless = true;
        } else if (flag == "--bench-once") {
            result.benchOnce = true;
        } else {  // --log, --bench y --bench-app llevan un valor
            if (i + 1 == args.size() || isFlag(args[i + 1])) {
                throw CliError(flag == "--log"     ? "--log necesita la ruta del archivo de log"
                               : flag == "--bench" ? "--bench necesita la ruta del archivo CSV"
                                                   : "--bench-app necesita el nombre de una aplicación");
            }
            (flag == "--log" ? result.logPath : flag == "--bench" ? result.benchPath : result.benchApp) = args[++i];
        }
    }
    if (!result.benchApp.empty() && !result.bench()) {
        throw CliError("--bench-app solo sirve con --bench o --bench-once");
    }
    return result;
}

const char* usage()
{
    return "uso: delivery_sim <config.json> [--log <path>] [--headless] [--bench <csv>] [--bench-once] [--bench-app <nombre>]\n";
}
