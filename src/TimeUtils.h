#pragma once

#include <Arduino.h>
#include <time.h>

namespace TimeUtils {

inline bool isUnixTimeValid(time_t t) {
    // 2020-09-13T12:26:40Z
    return t > 1600000000;
}

inline bool isTimeValidNow() {
    return isUnixTimeValid(time(nullptr));
}

inline uint32_t nowUnixSecondsOrZero() {
    time_t t = time(nullptr);
    return isUnixTimeValid(t) ? (uint32_t)t : 0;
}

inline uint32_t nowSecondsPreferUnix() {
    uint32_t unixNow = nowUnixSecondsOrZero();
    if (unixNow != 0) return unixNow;
    return millis() / 1000;
}

inline uint32_t unixFromUptimeSeconds(uint32_t uptimeSeconds) {
    uint32_t unixNow = nowUnixSecondsOrZero();
    if (unixNow == 0) return 0;

    uint32_t upNow = millis() / 1000;
    if (upNow >= uptimeSeconds) {
        return unixNow - (upNow - uptimeSeconds);
    }
    return unixNow + (uptimeSeconds - upNow);
}

inline bool looksLikeUnixSeconds(uint32_t ts) {
    return ts >= 1600000000UL;
}

inline String isoUtcFromUnixSeconds(uint32_t unixSeconds) {
    if (!looksLikeUnixSeconds(unixSeconds)) return String();

    time_t t = (time_t)unixSeconds;
    struct tm tmUtc;
    gmtime_r(&t, &tmUtc);

    char buf[21];
    // YYYY-MM-DDTHH:MM:SSZ
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmUtc.tm_year + 1900,
             tmUtc.tm_mon + 1,
             tmUtc.tm_mday,
             tmUtc.tm_hour,
             tmUtc.tm_min,
             tmUtc.tm_sec);
    return String(buf);
}

inline String isoUtcNow() {
    return isoUtcFromUnixSeconds(nowUnixSecondsOrZero());
}

}  // namespace TimeUtils
