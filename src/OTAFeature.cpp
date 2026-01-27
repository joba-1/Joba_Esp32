#include "OTAFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>

OTAFeature::OTAFeature(const char* hostname, const char* password, uint16_t port)
    : _hostname(hostname)
    , _password(password)
    , _port(port)
    , _state(State::WAITING_FOR_WIFI)
    , _ready(false)
    , _setupDone(false)
{
}

void OTAFeature::setup() {
    if (_setupDone) return;
    
    LOG_I("OTA configured, hostname: %s, port: %d", _hostname, _port);
    
    _setupDone = true;
    _state = State::WAITING_FOR_WIFI;
}

void OTAFeature::loop() {
    switch (_state) {
        case State::WAITING_FOR_WIFI:
            // Wait for WiFi connection before initializing OTA
            if (WiFi.status() == WL_CONNECTED) {
                _state = State::INITIALIZING;
            }
            break;
            
        case State::INITIALIZING:
            {
                LOG_I("Initializing OTA...");
                
                // Set hostname
                ArduinoOTA.setHostname(_hostname);
                
                // Set port
                ArduinoOTA.setPort(_port);
                
                // Set password if provided
                if (strlen(_password) > 0) {
                    ArduinoOTA.setPassword(_password);
                }
                
                // Setup callbacks
                ArduinoOTA.onStart([]() {
                    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
                    LOG_I("OTA Start: %s", type.c_str());
                });
                
                ArduinoOTA.onEnd([]() {
                    LOG_I("OTA End - Rebooting...");
                });
                
                ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
                    static int lastPercent = -1;
                    int percent = (progress / (total / 100));
                    if (percent != lastPercent && percent % 10 == 0) {
                        LOG_I("OTA Progress: %d%%", percent);
                        lastPercent = percent;
                    }
                });
                
                ArduinoOTA.onError([](ota_error_t error) {
                    const char* errorMsg = "Unknown";
                    switch (error) {
                        case OTA_AUTH_ERROR: errorMsg = "Auth Failed"; break;
                        case OTA_BEGIN_ERROR: errorMsg = "Begin Failed"; break;
                        case OTA_CONNECT_ERROR: errorMsg = "Connect Failed"; break;
                        case OTA_RECEIVE_ERROR: errorMsg = "Receive Failed"; break;
                        case OTA_END_ERROR: errorMsg = "End Failed"; break;
                    }
                    LOG_E("OTA Error: %s", errorMsg);
                });
                
                // Start OTA service
                ArduinoOTA.begin();
                
                _ready = true;
                _state = State::READY;
                
                LOG_I("OTA ready at %s.local:%d", _hostname, _port);
            }
            break;
            
        case State::READY:
            // Handle OTA requests (non-blocking)
            ArduinoOTA.handle();
            
            // Check if WiFi lost
            if (WiFi.status() != WL_CONNECTED) {
                _ready = false;
                _state = State::WAITING_FOR_WIFI;
                LOG_W("WiFi lost, OTA disabled");
            }
            break;
            
        case State::UPDATING:
            // During update, just handle OTA
            ArduinoOTA.handle();
            break;
    }
}
