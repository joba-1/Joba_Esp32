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
    // Initialize the monitor. Safe to call multiple times.
    void init();

    // Call at the START of each main loop iteration.
    // The time since last markLoopEnd() is counted as idle time.
    void markLoopStart();

    // Call at the END of each main loop iteration.
    // The time since markLoopStart() is counted as busy time.
    void markLoopEnd();

    // Returns average CPU usage (0.0 - 100.0) over the measurement window.
    float usagePercent();

    // Returns total busy time in the current/last window (microseconds).
    uint32_t busyTimeUs();

    // Returns total idle time in the current/last window (microseconds).
    uint32_t idleTimeUs();

    // Returns the loop count during the current/last measurement window.
    uint32_t loopCount();

    // Returns average loop duration in microseconds (busy time / loop count).
    uint32_t avgLoopDurationUs();

    // Reset all accumulated stats.
    void reset();

    // Enable periodic logging of CPU stats at specified interval (milliseconds).
    // Set interval to 0 to disable. Default: disabled.
    void setLogInterval(uint32_t intervalMs);
}
