#pragma once

#include <string>

// Minimal test-only String shim to avoid depending on Arduino types
namespace InfluxLineProtocol {
class String {
public:
    String() = default;
    String(const char* s) { if (s) _s = s; }
    String(const std::string& s): _s(s) {}
    String& operator+=(char c) { _s.push_back(c); return *this; }
    String& operator+=(const char* s) { if (s) _s += s; return *this; }
    const char* c_str() const { return _s.c_str(); }
    std::string str() const { return _s; }
    bool operator==(const char* o) const { return _s == (o? o : std::string()); }
private:
    std::string _s;
};

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
        if (c == '\\' || c == ',' || c == ' ') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace InfluxLineProtocol
