#include "TimeSyncFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>
#include <time.h>

TimeSyncFeature::TimeSyncFeature(const char* ntpServer1, const char* ntpServer2,
                                 const char* timezone, uint32_t syncIntervalMs)
    : _ntpServer1(ntpServer1)
    , _ntpServer2(ntpServer2)
    , _timezone(timezone)
    , _syncIntervalMs(syncIntervalMs)
    , _state(State::WAITING_FOR_WIFI)
    , _synced(false)
    , _setupDone(false)
    , _lastSyncTime(0)
    , _syncStartTime(0)
{
}

void TimeSyncFeature::setup() {
    if (_setupDone) return;
    
    LOG_I("Configuring timezone: %s", _timezone);
    
    // Configure timezone
    setenv("TZ", _timezone, 1);
    tzset();
    
    _setupDone = true;
    _state = State::WAITING_FOR_WIFI;
    
    LOG_I("TimeSync configured, waiting for WiFi...");
}

void TimeSyncFeature::loop() {
    switch (_state) {
        case State::WAITING_FOR_WIFI:
            // Wait for WiFi connection before attempting NTP sync
            if (WiFi.status() == WL_CONNECTED) {
                LOG_I("Starting NTP sync with %s", _ntpServer1);
                configTime(0, 0, _ntpServer1, _ntpServer2);
                _syncStartTime = millis();
                _state = State::SYNCING;
            }
            break;
            
        case State::SYNCING:
            {
                struct tm timeinfo;
                // Non-blocking check - timeout of 0
                if (getLocalTime(&timeinfo, 0)) {
                    _synced = true;
                    _lastSyncTime = millis();
                    _state = State::SYNCED;
                    LOG_I("Time synchronized: %s", getFormattedTime().c_str());
                } else if (millis() - _syncStartTime > SYNC_TIMEOUT) {
                    // Timeout - go back to waiting
                    LOG_W("NTP sync timeout, will retry...");
                    _state = State::WAITING_FOR_WIFI;
                }
            }
            break;
            
        case State::SYNCED:
            // Check if it's time to re-sync
            if (millis() - _lastSyncTime >= _syncIntervalMs) {
                _state = State::RESYNC_PENDING;
            }
            
            // Check if WiFi lost
            if (WiFi.status() != WL_CONNECTED) {
                LOG_W("WiFi lost, time sync may drift");
            }
            break;
            
        case State::RESYNC_PENDING:
            if (WiFi.status() == WL_CONNECTED) {
                LOG_D("Re-syncing time with NTP...");
                configTime(0, 0, _ntpServer1, _ntpServer2);
                _syncStartTime = millis();
                _state = State::SYNCING;
            } else {
                // No WiFi, stay synced but can't resync
                _state = State::SYNCED;
            }
            break;
    }
}

bool TimeSyncFeature::isSynced() const {
    return _synced;
}

String TimeSyncFeature::getFormattedTime() const {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) {
        return "Time not synced";
    }
    
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

time_t TimeSyncFeature::getEpochTime() const {
    time_t now;
    time(&now);
    return now;
}
