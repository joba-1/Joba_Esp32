#pragma once

#include <Arduino.h>
#include <time.h>

/**
 * @file TimeUtils.h
 * @brief Small time helper utilities used across the firmware
 *
 * Provides convenience functions for dealing with Unix timestamps and
 * formatting ISO-8601 UTC strings. These helpers prefer real Unix time
 * when available and fall back to uptime-based timestamps otherwise.
 */
namespace TimeUtils {

/**
 * @brief Validate that an epoch looks like a real Unix time (post-2020)
 */
inline bool isUnixTimeValid(time_t t) {
    // 2020-09-13T12:26:40Z
    return t > 1600000000;
}

/**
 * @return true when `t` looks like a valid Unix epoch (post-2020), false otherwise.
 */

/**
 * @brief True when the system clock appears to be set to a valid Unix time
 */
inline bool isTimeValidNow() {
    return isUnixTimeValid(time(nullptr));
}

/**
 * @return true if the system clock currently appears to be valid Unix time.
 */

/**
 * @brief Return current Unix seconds or zero if clock is unset
 */
inline uint32_t nowUnixSecondsOrZero() {
    time_t t = time(nullptr);
    return isUnixTimeValid(t) ? (uint32_t)t : 0;
}

/**
 * @return current Unix seconds if available, otherwise 0.
 */

/**
 * @brief Prefer real Unix seconds, fall back to uptime seconds
 */
inline uint32_t nowSecondsPreferUnix() {
    uint32_t unixNow = nowUnixSecondsOrZero();
    if (unixNow != 0) return unixNow;
    return millis() / 1000;
}

/**
 * @return current seconds preferring real Unix time, or uptime seconds.
 */

/**
 * @brief Convert an uptime-based seconds value to Unix seconds when possible
 * @param uptimeSeconds Seconds since boot for the event
 * @return Unix epoch seconds or 0 when real time is not available
 */
inline uint32_t unixFromUptimeSeconds(uint32_t uptimeSeconds) {
    uint32_t unixNow = nowUnixSecondsOrZero();
    if (unixNow == 0) return 0;

    uint32_t upNow = millis() / 1000;
    if (upNow >= uptimeSeconds) {
        return unixNow - (upNow - uptimeSeconds);
    }
    return unixNow + (uptimeSeconds - upNow);
}

/**
 * @brief Convert an uptime-based seconds value to Unix seconds when possible
 * @param uptimeSeconds Seconds since boot for the event
 * @return Unix epoch seconds or 0 when real time is not available
 */

/**
 * @brief Heuristic whether a 32-bit timestamp looks like UNIX seconds
 */
inline bool looksLikeUnixSeconds(uint32_t ts) {
    return ts >= 1600000000UL;
}

/**
 * @return true if `ts` plausibly represents Unix epoch seconds (>= 2020).
 */

/**
 * @brief Format Unix seconds as ISO-8601 UTC string (YYYY-MM-DDTHH:MM:SSZ)
 * @param unixSeconds Seconds since the Unix epoch
 * @return Empty `String` when the input does not look like Unix seconds
 */
inline String isoUtcFromUnixSeconds(uint32_t unixSeconds) {
    if (!looksLikeUnixSeconds(unixSeconds)) return String();


/**
 * @brief ISO-8601 UTC string for now (prefers real Unix time)
 * @return ISO-8601 UTC string or empty String when time is unavailable
 */
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

/**
 * @brief ISO-8601 UTC string for now (prefers real Unix time)
 */
inline String isoUtcNow() {
    return isoUtcFromUnixSeconds(nowUnixSecondsOrZero());
}

}  // namespace TimeUtils
