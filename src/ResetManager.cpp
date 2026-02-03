#include "ResetManager.h"
#include "LoggingFeature.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
    volatile bool g_restartScheduled = false;

    struct RestartTaskArgs {
        uint32_t delayMs;
    };

    void restartTask(void* pv) {
        RestartTaskArgs* args = static_cast<RestartTaskArgs*>(pv);
        const uint32_t delayMs = args ? args->delayMs : 200;
        delete args;

        if (delayMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }

        ESP.restart();
        vTaskDelete(nullptr);
    }

    uint32_t clampDelay(uint32_t delayMs) {
        // Ensure we can return HTTP/MQTT acks before resetting.
        static constexpr uint32_t MIN_MS = 50;
        static constexpr uint32_t MAX_MS = 10000;
        if (delayMs < MIN_MS) return MIN_MS;
        if (delayMs > MAX_MS) return MAX_MS;
        return delayMs;
    }
}

bool ResetManager::scheduleRestart(uint32_t delayMs, const char* reason) {
    if (g_restartScheduled) return false;

    g_restartScheduled = true;

    const uint32_t ms = clampDelay(delayMs);
    LOG_W("Restart scheduled in %ums (%s)", (unsigned)ms, reason ? reason : "unknown");

    auto* args = new RestartTaskArgs{ms};
    if (!args) {
        LOG_E("Restart schedule failed: OOM");
        g_restartScheduled = false;
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        restartTask,
        "restart",
        2048,
        args,
        1,
        nullptr,
        tskNO_AFFINITY
    );

    if (ok != pdPASS) {
        LOG_E("Restart schedule failed: xTaskCreatePinnedToCore");
        delete args;
        g_restartScheduled = false;
        return false;
    }

    return true;
}

bool ResetManager::isRestartScheduled() {
    return g_restartScheduled;
}
