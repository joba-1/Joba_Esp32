#include "CpuMonitor.h"
#include "LoggingFeature.h"
#include <string.h>

/**
 * @file CpuMonitor.cpp
 * @brief Lightweight CPU usage and loop-time measurement helpers.
 *
 * This translation unit implements a small, non-blocking CPU monitor used by
 * the main loop to record busy/idle time, loop counts and average loop
 * durations. The implementation is deliberately allocation-free and
 * designed for frequent calls from the primary `loop()` path.
 */

namespace {
    // Measurement window (how often stats roll over)
    static constexpr uint32_t WINDOW_US = 1000000;  // 1 second

    bool s_initialized = false;

    // Logging state
    uint32_t s_logIntervalMs = 0;  // 0 = disabled
    uint32_t s_lastLogMs = 0;

    // Current measurement window accumulators
    uint32_t s_windowStartUs = 0;
    uint32_t s_busyAccumUs = 0;
    uint32_t s_idleAccumUs = 0;
    uint32_t s_loopCount = 0;

    // Previous window results (stable values for queries)
    uint32_t s_lastBusyUs = 0;
    uint32_t s_lastIdleUs = 0;
    uint32_t s_lastLoopCount = 0;

    // Timing state
    uint32_t s_loopStartUs = 0;
    uint32_t s_loopEndUs = 0;
    bool s_inLoop = false;

    // Per-feature statistics
    static constexpr size_t MAX_FEATURE_STATS = 32;
    struct FeatureStat {
        const char* name = nullptr;
        uint32_t minUs = 0;
        uint32_t maxUs = 0;
        uint64_t sumUs = 0;
        uint32_t count = 0;
    };

    FeatureStat s_currStats[MAX_FEATURE_STATS];
    FeatureStat s_lastStats[MAX_FEATURE_STATS];
    size_t s_currStatCount = 0;
    size_t s_lastStatCount = 0;

    void rollWindow() {
        // Save current window to "last" results
        s_lastBusyUs = s_busyAccumUs;
        s_lastIdleUs = s_idleAccumUs;
        s_lastLoopCount = s_loopCount;

        // Snapshot per-feature stats
        s_lastStatCount = s_currStatCount;
        for (size_t i = 0; i < s_lastStatCount && i < MAX_FEATURE_STATS; ++i) {
            s_lastStats[i] = s_currStats[i];
        }

        // Reset current per-feature accumulators
        for (size_t i = 0; i < MAX_FEATURE_STATS; ++i) {
            s_currStats[i].name = nullptr;
            s_currStats[i].minUs = 0;
            s_currStats[i].maxUs = 0;
            s_currStats[i].sumUs = 0;
            s_currStats[i].count = 0;
        }
        s_currStatCount = 0;

        // Reset accumulators for new window
        s_busyAccumUs = 0;
        s_idleAccumUs = 0;
        s_loopCount = 0;
        s_windowStartUs = (uint32_t)micros();
    }
}

namespace CpuMonitor {
    void init() {
        if (s_initialized) return;
        s_initialized = true;
        s_windowStartUs = (uint32_t)micros();
        s_loopEndUs = s_windowStartUs;
    }

    // Note: the implementation below is intentionally lightweight and
    // uses simple micros()/millis() arithmetic. It is safe to call from
    // the main loop and avoids heap allocations.

    void markLoopStart() {
        init();
        const uint32_t now = (uint32_t)micros();

        // Time since last loop end = idle time
        if (s_loopEndUs != 0) {
            uint32_t idle = now - s_loopEndUs;
            s_idleAccumUs += idle;
        }

        s_loopStartUs = now;
        s_inLoop = true;
    }

    void markLoopEnd() {
        if (!s_inLoop) return;

        const uint32_t now = (uint32_t)micros();

        // Time since loop start = busy time
        uint32_t busy = now - s_loopStartUs;
        s_busyAccumUs += busy;
        s_loopCount++;

        s_loopEndUs = now;
        s_inLoop = false;

        // Check if measurement window has elapsed
        if ((now - s_windowStartUs) >= WINDOW_US) {
            rollWindow();

            // Periodic logging if enabled
            if (s_logIntervalMs > 0) {
                const uint32_t nowMs = (uint32_t)millis();
                if ((nowMs - s_lastLogMs) >= s_logIntervalMs) {
                    s_lastLogMs = nowMs;
                    LOG_I("CPU: %.1f%%, loops/s=%u, avgLoop=%uus, heap=%u",
                          usagePercent(), s_lastLoopCount, avgLoopDurationUs(),
                          (unsigned)ESP.getFreeHeap());

                    // Log per-feature min/avg/max for last window
                    const char* subNames[] = {"ModbusRX", "ModbusParse", "ModbusQueue", "ModbusScan", "ModbusWait", "ModbusParseUnit", "ModbusUpdateMap", "ModbusTX"};
                    for (size_t i = 0; i < s_lastStatCount && i < MAX_FEATURE_STATS; ++i) {
                        const FeatureStat& st = s_lastStats[i];
                        if (!st.name || st.count == 0) continue;

                        // Skip printing sub-components at top-level; they'll be shown indented under ModbusRTU
                        bool isSub = false;
                        for (const char* sub : subNames) {
                            if (strcmp(st.name, sub) == 0) { isSub = true; break; }
                        }
                        if (isSub) continue;

                        uint32_t avg = (uint32_t)(st.sumUs / st.count);
                        LOG_I(" Feature %-12s min=%3uus avg=%3uus max=%3uus",
                              st.name, st.minUs, avg, st.maxUs);

                        // If this is the ModbusRTU feature, also print sub-component timings
                        if (strcmp(st.name, "ModbusRTU") == 0) {
                            for (const char* sub : subNames) {
                                for (size_t j = 0; j < s_lastStatCount && j < MAX_FEATURE_STATS; ++j) {
                                    const FeatureStat& subSt = s_lastStats[j];
                                    if (!subSt.name || subSt.count == 0) continue;
                                    if (strcmp(subSt.name, sub) == 0) {
                                        uint32_t subAvg = (uint32_t)(subSt.sumUs / subSt.count);
                                        LOG_I("         %-12s min=%3uus avg=%3uus max=%3uus",
                                              sub, subSt.minUs, subAvg, subSt.maxUs);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void recordFeatureDuration(const char* name, uint32_t durUs) {
        if (!name) return;

        // Try to find existing entry
        for (size_t i = 0; i < s_currStatCount; ++i) {
            FeatureStat& st = s_currStats[i];
            if (st.name == name || (st.name && strcmp(st.name, name) == 0)) {
                if (st.count == 0) {
                    st.minUs = st.maxUs = durUs;
                    st.sumUs = durUs;
                    st.count = 1;
                } else {
                    if (durUs < st.minUs) st.minUs = durUs;
                    if (durUs > st.maxUs) st.maxUs = durUs;
                    st.sumUs += durUs;
                    st.count++;
                }
                return;
            }
        }

        // Add new entry if space
        if (s_currStatCount < MAX_FEATURE_STATS) {
            FeatureStat& st = s_currStats[s_currStatCount++];
            st.name = name;
            st.minUs = durUs;
            st.maxUs = durUs;
            st.sumUs = durUs;
            st.count = 1;
        }
    }

    float usagePercent() {
        uint32_t busy = s_lastBusyUs;
        uint32_t idle = s_lastIdleUs;
        uint32_t total = busy + idle;
        if (total == 0) {
            // No data yet; return current window estimate
            busy = s_busyAccumUs;
            idle = s_idleAccumUs;
            total = busy + idle;
        }
        if (total == 0) return 0.0f;
        return (float)busy * 100.0f / (float)total;
    }

    uint32_t busyTimeUs() {
        return s_lastBusyUs > 0 ? s_lastBusyUs : s_busyAccumUs;
    }

    uint32_t idleTimeUs() {
        return s_lastIdleUs > 0 ? s_lastIdleUs : s_idleAccumUs;
    }

    uint32_t loopCount() {
        return s_lastLoopCount > 0 ? s_lastLoopCount : s_loopCount;
    }

    uint32_t avgLoopDurationUs() {
        uint32_t busy = busyTimeUs();
        uint32_t count = loopCount();
        return count > 0 ? busy / count : 0;
    }

    void reset() {
        s_busyAccumUs = 0;
        s_idleAccumUs = 0;
        s_loopCount = 0;
        s_lastBusyUs = 0;
        s_lastIdleUs = 0;
        s_lastLoopCount = 0;
        s_windowStartUs = (uint32_t)micros();
        s_loopEndUs = s_windowStartUs;
        s_inLoop = false;
    }

    void setLogInterval(uint32_t intervalMs) {
        s_logIntervalMs = intervalMs;
        s_lastLogMs = (uint32_t)millis();
    }

    bool getLastFeatureStats(const char* name, uint32_t& outMinUs, uint32_t& outAvgUs, uint32_t& outMaxUs) {
        if (!name) return false;
        for (size_t i = 0; i < s_lastStatCount && i < MAX_FEATURE_STATS; ++i) {
            const FeatureStat& st = s_lastStats[i];
            if (!st.name) continue;
            if (strcmp(st.name, name) == 0 && st.count > 0) {
                outMinUs = st.minUs;
                outAvgUs = (uint32_t)(st.sumUs / st.count);
                outMaxUs = st.maxUs;
                return true;
            }
        }
        return false;
    }
}
