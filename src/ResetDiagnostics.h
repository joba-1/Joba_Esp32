#pragma once

#include <Arduino.h>

#include <esp_system.h>

namespace ResetDiagnostics {
    // Call once early in setup(). Safe to call multiple times.
    void init();

    // Stores a small "breadcrumb" in RTC memory so after a crash/reboot we can
    // inspect what the firmware was doing last.
    //
    // phase examples: "setup", "loop", "job"
    // name examples:  Feature::getName(), "collectSensorData", "modbusDevices"
    void setBreadcrumb(const char* phase, const char* name);

    // Records timing information (microseconds) for the main loop, persisted in RTC
    // memory so it can be inspected after watchdog resets.
    void recordLoopDurationUs(const char* name, uint32_t durationUs);

    uint32_t bootCount();

    esp_reset_reason_t resetReason();
    const char* resetReasonString();

    // Per-core RTC reset reason codes (ROM). 0 when unavailable.
    uint32_t rtcResetReasonCore0();
    uint32_t rtcResetReasonCore1();

    const char* breadcrumbPhase();
    const char* breadcrumbName();
    uint32_t breadcrumbUptimeMs();

    const char* lastLoopName();
    uint32_t lastLoopDurationUs();

    const char* maxLoopName();
    uint32_t maxLoopDurationUs();
}
