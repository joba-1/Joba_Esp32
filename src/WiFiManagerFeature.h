#ifndef WIFIMANAGER_FEATURE_H
#define WIFIMANAGER_FEATURE_H

#include <Arduino.h>
#include <WiFiManager.h>
#include "Feature.h"

/**
 * @file WiFiManagerFeature.h
 * @brief WiFi connection management with captive portal for configuration
 *
 * Wraps `WiFiManager` to provide non-blocking configuration portal support
 * and simple reconnect logic used by the firmware.
 */
class WiFiManagerFeature : public Feature {
public:
    /**
     * @brief Construct WiFiManager feature
     * @param apName Access point name for configuration portal
     * @param apPassword Access point password (empty string for open AP)
     * @param configPortalTimeout Timeout in seconds for config portal
     */
    WiFiManagerFeature(const char* apName, const char* apPassword, uint16_t configPortalTimeout);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "WiFiManager"; }
    bool isReady() const override { return _connected; }
    
    /**
     * @brief True when WiFi is currently connected
     */
    bool isConnected() const;

    /**
     * @brief Return current IP address as string or "0.0.0.0" when offline
     */
    String getIPAddress() const;
    
    // Setters for dynamic configuration
    void setAPName(const char* apName) { _apName = apName; }
    void setAPPassword(const char* apPassword) { _apPassword = apPassword; }
    
private:
    enum class State {
        IDLE,
        CONNECTING,
        CONNECTED,
        DISCONNECTED
    };
    
    const char* _apName;
    const char* _apPassword;
    uint16_t _configPortalTimeout;
    
    WiFiManager _wifiManager;
    State _state;
    bool _connected;
    bool _setupDone;
    unsigned long _lastReconnectAttempt;
    static const unsigned long RECONNECT_INTERVAL = 30000; // 30 seconds
};

#endif // WIFIMANAGER_FEATURE_H
