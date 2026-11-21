#include "cli.hpp"

#include <map>
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
        // Las que llevan un valor, con lo que dice el error si falta.
        static const std::map<std::string, std::string> withValue = {
            {"--log", "--log necesita la ruta del archivo de log"},
            {"--bench", "--bench necesita la ruta del archivo CSV"},
            {"--bench-app", "--bench-app necesita el nombre de una aplicación"},
            {"--report", "--report necesita la ruta del archivo del reporte"},
        };
        if (flag != "--headless" && flag != "--bench-once" && flag != "--frames" && withValue.count(flag) == 0) {
            throw CliError("argumento desconocido '" + flag + "'");
        }
        if (!seen.insert(flag).second) {
            throw CliError(flag + " aparece más de una vez");
        }
        if (flag == "--headless") {
            result.headless = true;
        } else if (flag == "--bench-once") {
            result.benchOnce = true;
        } else if (flag == "--frames") {
            result.frames = true;
        } else {
            if (i + 1 == args.size() || isFlag(args[i + 1])) {
                throw CliError(withValue.at(flag));
            }
            const std::string& value = args[++i];
            if (flag == "--log") {
                result.logPath = value;
            } else if (flag == "--bench") {
                result.benchPath = value;
            } else if (flag == "--bench-app") {
                result.benchApp = value;
            } else {
                result.reportPath = value;
            }
        }
    }
    if (!result.benchApp.empty() && !result.bench()) {
        throw CliError("--bench-app solo sirve con --bench o --bench-once");
    }
    if (!result.reportPath.empty() && result.bench()) {
        throw CliError("--report es de la simulación: no se combina con --bench ni --bench-once");
    }
    if (result.frames && result.bench()) {
        throw CliError("--frames es de la simulación: no se combina con --bench ni --bench-once");
    }
    return result;
}

const char* usage()
{
    return "uso: delivery_sim <config.json> [--log <path>] [--headless] [--report <json>]\n"
           "     delivery_sim <config.json> --bench <csv> | --bench-once [--bench-app <nombre>]\n";
}
