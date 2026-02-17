#pragma once

#include <Arduino.h>

/**
 * Simple CPU usage monitor for ESP32.
 * 
 * Tracks busy vs idle time in the main loop to estimate CPU utilization.
 * Uses a sliding window to compute average usage percentage.
 * 
 * Usage:
 *   In loop():
 *     CpuMonitor::markLoopStart();
 *     // ... do work ...
 *     CpuMonitor::markLoopEnd();
 * 
 *   Query:
 *     float usage = CpuMonitor::usagePercent();
 */
namespace CpuMonitor {
    /**
     * @brief Initialize the CPU monitor subsystem
     *
     * Safe to call multiple times; prepares internal timing buffers.
     */
    void init();

    /**
     * @brief Mark the start of a main-loop iteration
     *
     * Call at the very beginning of `loop()`. The time between the previous
     * `markLoopEnd()` and this call is accounted as idle time.
     */
    void markLoopStart();

    /**
     * @brief Mark the end of a main-loop iteration
     *
     * Call at the end of `loop()`. The time between the matching
     * `markLoopStart()` and this call is counted as busy time.
     */
    void markLoopEnd();

    /**
     * @brief Average CPU usage percentage over the configured window
     * @return Value in range 0.0 - 100.0
     */
    float usagePercent();

    /** @return Total busy time in current/last window (microseconds) */
    uint32_t busyTimeUs();

    /** @return Total idle time in current/last window (microseconds) */
    uint32_t idleTimeUs();

    /** @return Loop iterations counted in current/last window */
    uint32_t loopCount();

    /** @return Average loop duration in microseconds (busyTime / loopCount) */
    uint32_t avgLoopDurationUs();

    /** @brief Reset accumulated statistics and timers */
    void reset();

    /**
     * @brief Enable/disable periodic logging of CPU stats
     * @param intervalMs Interval in milliseconds; 0 to disable
     */
    void setLogInterval(uint32_t intervalMs);
    
    /**
     * @brief Record a single feature loop duration for per-feature stats
     * @param name   Feature name (pointer is stored; must remain valid)
     * @param durUs  Duration in microseconds
     */
    void recordFeatureDuration(const char* name, uint32_t durUs);
}

