#pragma once

// Funciones compartidas por las pruebas para leer líneas del log de eventos.

#include <cstdint>
#include <iostream>
#include <string>

// Valor del campo de texto `key` en una línea del log ("" si no está).
inline std::string field(const std::string& line, const std::string& key)
{
    const std::string open = "\"" + key + "\": \"";
    const auto start = line.find(open);
    if (start == std::string::npos) {
        return "";
    }
    const auto begin = start + open.size();
    return line.substr(begin, line.find('"', begin) - begin);
}

// El `t` de una línea del log: es siempre el primer campo.
inline std::int64_t timeOf(const std::string& line) { return std::stoll(line.substr(line.find(':') + 1)); }

// Imprime "nombre: OK" o "nombre: FALLA" y devuelve el resultado.
inline bool check(const char* name, bool ok)
{
    std::cout << name << ": " << (ok ? "OK" : "FALLA") << "\n";
    return ok;
}
