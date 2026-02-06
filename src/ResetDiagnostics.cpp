#include "ResetDiagnostics.h"

#include <esp_system.h>

#include <string.h>

#include <esp_attr.h>

// rtc_get_reset_reason() lives in ROM headers.
#include <rom/rtc.h>

namespace {
    struct RtcState {
        uint32_t magic;
        uint32_t bootCount;
        uint32_t breadcrumbUptimeMs;
        char breadcrumbPhase[8];
        char breadcrumbName[24];

        uint32_t lastLoopDurationUs;
        char lastLoopName[24];

        uint32_t maxLoopDurationUs;
        char maxLoopName[24];
    };

    // RTC memory persists across soft resets (not power loss).
    // Use NOINIT so it isn't cleared/reinitialized on boot.
    RTC_NOINIT_ATTR RtcState s_rtcState;

    static constexpr uint32_t RTC_MAGIC = 0x52455354; // 'REST'

    bool s_initialized = false;
    esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
    uint32_t s_rtc0 = 0;
    uint32_t s_rtc1 = 0;

    const char* resetReasonToString(esp_reset_reason_t reason) {
        switch (reason) {
            case ESP_RST_UNKNOWN:   return "unknown";
            case ESP_RST_POWERON:   return "poweron";
            case ESP_RST_EXT:       return "external";
            case ESP_RST_SW:        return "software";
            case ESP_RST_PANIC:     return "panic";
            case ESP_RST_INT_WDT:   return "int_wdt";
            case ESP_RST_TASK_WDT:  return "task_wdt";
            case ESP_RST_WDT:       return "wdt";
            case ESP_RST_DEEPSLEEP: return "deepsleep";
            case ESP_RST_BROWNOUT:  return "brownout";
            case ESP_RST_SDIO:      return "sdio";
            default:               return "other";
        }
    }
}

namespace ResetDiagnostics {
    void init() {
        if (s_initialized) return;
        s_initialized = true;

        if (s_rtcState.magic != RTC_MAGIC) {
            memset(&s_rtcState, 0, sizeof(s_rtcState));
            s_rtcState.magic = RTC_MAGIC;
        }

        // Increment early; helps detect boot loops.
        s_rtcState.bootCount++;

        s_reason = esp_reset_reason();

        // Best-effort; these are uint32 codes defined by ROM.
        s_rtc0 = (uint32_t)rtc_get_reset_reason(0);
        s_rtc1 = (uint32_t)rtc_get_reset_reason(1);

        // Ensure breadcrumb strings are always terminated.
        s_rtcState.breadcrumbPhase[sizeof(s_rtcState.breadcrumbPhase) - 1] = '\0';
        s_rtcState.breadcrumbName[sizeof(s_rtcState.breadcrumbName) - 1] = '\0';

        s_rtcState.lastLoopName[sizeof(s_rtcState.lastLoopName) - 1] = '\0';
        s_rtcState.maxLoopName[sizeof(s_rtcState.maxLoopName) - 1] = '\0';
    }

    void setBreadcrumb(const char* phase, const char* name) {
        init();
        if (!phase) phase = "";
        if (!name) name = "";

        const uint32_t now = (uint32_t)millis();

        // Avoid excessive RTC writes in tight loops.
        // Always update if phase/name changed; otherwise rate-limit.
        const bool phaseSame = strncmp(s_rtcState.breadcrumbPhase, phase, sizeof(s_rtcState.breadcrumbPhase)) == 0;
        const bool nameSame = strncmp(s_rtcState.breadcrumbName, name, sizeof(s_rtcState.breadcrumbName)) == 0;
        if (phaseSame && nameSame) {
            if ((uint32_t)(now - s_rtcState.breadcrumbUptimeMs) < 250) return;
        }

        strncpy(s_rtcState.breadcrumbPhase, phase, sizeof(s_rtcState.breadcrumbPhase) - 1);
        s_rtcState.breadcrumbPhase[sizeof(s_rtcState.breadcrumbPhase) - 1] = '\0';

        strncpy(s_rtcState.breadcrumbName, name, sizeof(s_rtcState.breadcrumbName) - 1);
        s_rtcState.breadcrumbName[sizeof(s_rtcState.breadcrumbName) - 1] = '\0';

        s_rtcState.breadcrumbUptimeMs = now;
    }

    void recordLoopDurationUs(const char* name, uint32_t durationUs) {
        init();
        if (!name) name = "";

        s_rtcState.lastLoopDurationUs = durationUs;
        strncpy(s_rtcState.lastLoopName, name, sizeof(s_rtcState.lastLoopName) - 1);
        s_rtcState.lastLoopName[sizeof(s_rtcState.lastLoopName) - 1] = '\0';

        if (durationUs >= s_rtcState.maxLoopDurationUs) {
            s_rtcState.maxLoopDurationUs = durationUs;
            strncpy(s_rtcState.maxLoopName, name, sizeof(s_rtcState.maxLoopName) - 1);
            s_rtcState.maxLoopName[sizeof(s_rtcState.maxLoopName) - 1] = '\0';
        }
    }

    uint32_t bootCount() {
        return s_rtcState.bootCount;
    }

    esp_reset_reason_t resetReason() {
        return s_reason;
    }

    const char* resetReasonString() {
        return resetReasonToString(s_reason);
    }

    uint32_t rtcResetReasonCore0() {
        return s_rtc0;
    }

    uint32_t rtcResetReasonCore1() {
        return s_rtc1;
    }

    const char* breadcrumbPhase() {
        return s_rtcState.breadcrumbPhase;
    }

    const char* breadcrumbName() {
        return s_rtcState.breadcrumbName;
    }

    uint32_t breadcrumbUptimeMs() {
        return s_rtcState.breadcrumbUptimeMs;
    }

    const char* lastLoopName() {
        return s_rtcState.lastLoopName;
    }

    uint32_t lastLoopDurationUs() {
        return s_rtcState.lastLoopDurationUs;
    }

    const char* maxLoopName() {
        return s_rtcState.maxLoopName;
    }

    uint32_t maxLoopDurationUs() {
        return s_rtcState.maxLoopDurationUs;
    }
}
