#include "CpuMonitor.h"
#include "LoggingFeature.h"

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

    void rollWindow() {
        // Save current window to "last" results
        s_lastBusyUs = s_busyAccumUs;
        s_lastIdleUs = s_idleAccumUs;
        s_lastLoopCount = s_loopCount;

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
                }
            }
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
}
