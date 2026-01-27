#ifndef TIMESYNC_FEATURE_H
#define TIMESYNC_FEATURE_H

#include <Arduino.h>
#include "Feature.h"

/**
 * @brief Time synchronization via NTP
 */
class TimeSyncFeature : public Feature {
public:
    /**
     * @brief Construct TimeSync feature
     * @param ntpServer1 Primary NTP server
     * @param ntpServer2 Secondary NTP server
     * @param timezone POSIX timezone string
     * @param syncIntervalMs Re-sync interval in milliseconds
     */
    TimeSyncFeature(const char* ntpServer1, const char* ntpServer2, 
                    const char* timezone, uint32_t syncIntervalMs);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "TimeSync"; }
    bool isReady() const override { return _synced; }
    
    bool isSynced() const;
    String getFormattedTime() const;
    time_t getEpochTime() const;
    
private:
    enum class State {
        WAITING_FOR_WIFI,
        SYNCING,
        SYNCED,
        RESYNC_PENDING
    };
    
    const char* _ntpServer1;
    const char* _ntpServer2;
    const char* _timezone;
    uint32_t _syncIntervalMs;
    
    State _state;
    bool _synced;
    bool _setupDone;
    unsigned long _lastSyncTime;
    unsigned long _syncStartTime;
    static const unsigned long SYNC_TIMEOUT = 10000; // 10 seconds timeout
};

#endif // TIMESYNC_FEATURE_H
