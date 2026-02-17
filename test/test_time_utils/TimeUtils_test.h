#pragma once

#include <time.h>
#include <string>

namespace TimeUtils {

inline bool isUnixTimeValid(time_t t) {
    return t > 1600000000;
}

inline bool looksLikeUnixSeconds(uint32_t ts) {
    return ts >= 1600000000UL;
}

inline std::string isoUtcFromUnixSeconds(uint32_t unixSeconds) {
    if (!looksLikeUnixSeconds(unixSeconds)) return std::string();
    time_t t = (time_t)unixSeconds;
    struct tm tmUtc;
    gmtime_r(&t, &tmUtc);
    char buf[21];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900,
             tmUtc.tm_mon + 1,
             tmUtc.tm_mday,
             tmUtc.tm_hour,
             tmUtc.tm_min,
             tmUtc.tm_sec);
    return std::string(buf);
}

} // namespace TimeUtils
