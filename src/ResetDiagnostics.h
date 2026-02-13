#pragma once

#include <Arduino.h>

#include <esp_system.h>

namespace ResetDiagnostics {
    /**
     * @brief Initialize reset diagnostics subsystem
     *
     * Safe to call multiple times. Should be invoked early from `setup()` so
     * breadcrumbs and RTC-backed diagnostics are available across reboots.
     */
    void init();

    /**
     * @brief Store a small breadcrumb in RTC memory
     *
     * Use this to record the current phase and a short name so that after a
     * crash or watchdog reset the last known activity is inspectable.
     *
     * @param phase Short phase string (e.g., "setup", "loop", "job")
     * @param name  Short name (e.g., Feature::getName(), "collectSensorData")
     */
    void setBreadcrumb(const char* phase, const char* name);

    /**
     * @brief Record last main-loop duration (microseconds)
     *
     * Persisted to RTC memory for post-mortem analysis after resets.
     *
     * @param name Identifier of loop/operation
     * @param durationUs Duration in microseconds
     */
    void recordLoopDurationUs(const char* name, uint32_t durationUs);

    /** @return Number of boots since power-on (approximate) */
    uint32_t bootCount();

    /** @return Platform reset reason enum */
    esp_reset_reason_t resetReason();
    /** @return Human-readable reset reason string */
    const char* resetReasonString();

    /**
     * @brief Per-core RTC reset reason codes (ROM values)
     * @return 0 when unavailable
     */
    uint32_t rtcResetReasonCore0();
    uint32_t rtcResetReasonCore1();

    /** @return Breadcrumb phase stored in RTC (or nullptr) */
    const char* breadcrumbPhase();
    /** @return Breadcrumb name stored in RTC (or nullptr) */
    const char* breadcrumbName();
    /** @return Uptime in milliseconds when breadcrumb was written */
    uint32_t breadcrumbUptimeMs();

    /** @return Name of last recorded loop/operation */
    const char* lastLoopName();
    /** @return Duration (us) of last recorded loop */
    uint32_t lastLoopDurationUs();

    /** @return Name of the longest observed loop */
    const char* maxLoopName();
    /** @return Duration (us) of the longest observed loop */
    uint32_t maxLoopDurationUs();
}
