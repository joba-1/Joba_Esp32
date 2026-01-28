#ifndef OTA_FEATURE_H
#define OTA_FEATURE_H

#include <Arduino.h>
#include <ArduinoOTA.h>
#include "Feature.h"

/**
 * @brief Over-the-air firmware updates via ArduinoOTA
 */
class OTAFeature : public Feature {
public:
    /**
     * @brief Construct OTA feature
     * @param hostname mDNS hostname for OTA discovery
     * @param password OTA password (empty = no password)
     * @param port OTA port
     */
    OTAFeature(const char* hostname, const char* password, uint16_t port);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "OTA"; }
    
    // Setter for dynamic configuration
    void setHostname(const char* hostname) { _hostname = hostname; }
    void setPassword(const char* password) { _password = password; }
    bool isReady() const override { return _ready; }
    
private:
    enum class State {
        WAITING_FOR_WIFI,
        INITIALIZING,
        READY,
        UPDATING
    };
    
    const char* _hostname;
    const char* _password;
    uint16_t _port;
    
    State _state;
    bool _ready;
    bool _setupDone;
};

#endif // OTA_FEATURE_H
