#pragma once

#include <Arduino.h>

// Minimal InfluxDB Line Protocol escaping helpers.
//
// Influx line protocol requires escaping the following characters in tag keys and tag values:
//   - commas (',')
//   - equals ('=')
//   - spaces (' ')
//
// Many implementations also escape backslashes to avoid ambiguous sequences.
namespace InfluxLineProtocol {

inline String escapeTag(const char* s) {
    if (!s) return String();

    String out;
    while (*s) {
        const char c = *s++;
        if (c == '\\' || c == ',' || c == '=' || c == ' ') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

inline String escapeTag(const String& s) {
    return escapeTag(s.c_str());
}

inline String escapeMeasurement(const char* s) {
    if (!s) return String();

    String out;
    while (*s) {
        const char c = *s++;
        // For measurement names: escape commas and spaces (and backslash for safety).
        if (c == '\\' || c == ',' || c == ' ') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

}  // namespace InfluxLineProtocol
