#pragma once

#include <Arduino.h>

/**
 * @file InfluxLineProtocol.h
 * @brief Small helpers for InfluxDB Line Protocol escaping
 *
 * Provides minimal helpers to escape measurement names and tag keys/values
 * so they may be safely emitted using InfluxDB line protocol. Functions are
 * intentionally lightweight and return `String` for easy use in the firmware.
 */
namespace InfluxLineProtocol {

/**
 * @brief Escape a tag key/value for InfluxDB line protocol
 * @param s NUL-terminated C string to escape
 * @return Escaped string as `String`
 */
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

/**
 * @brief Convenience overload for `escapeTag` accepting `String`
 */
inline String escapeTag(const String& s) {
    return escapeTag(s.c_str());
}

/**
 * @brief Escape a measurement name for InfluxDB line protocol
 * @param s NUL-terminated C string measurement name
 * @return Escaped measurement as `String`
 */
inline String escapeMeasurement(const char* s) {
    if (!s) return String();

    String out;
    while (*s) {
        const char c = *s++;
        if (c == '\\' || c == ',' || c == ' ') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

}  // namespace InfluxLineProtocol
