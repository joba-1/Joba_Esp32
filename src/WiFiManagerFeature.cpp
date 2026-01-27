#include "WiFiManagerFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>

WiFiManagerFeature::WiFiManagerFeature(const char* apName, const char* apPassword, uint16_t configPortalTimeout)
    : _apName(apName)
    , _apPassword(apPassword)
    , _configPortalTimeout(configPortalTimeout)
    , _state(State::IDLE)
    , _connected(false)
    , _setupDone(false)
    , _lastReconnectAttempt(0)
{
}

void WiFiManagerFeature::setup() {
    if (_setupDone) return;
    
    LOG_I("Configuring WiFiManager...");
    
    // Set timeout for config portal
    _wifiManager.setConfigPortalTimeout(_configPortalTimeout);
    
    // Set connection timeout
    _wifiManager.setConnectTimeout(20);
    
    // Don't block - use non-blocking autoConnect
    // If no saved credentials, this will start the config portal
    _wifiManager.setConfigPortalBlocking(false);
    
    // Start WiFi connection attempt
    if (strlen(_apPassword) > 0) {
        _wifiManager.autoConnect(_apName, _apPassword);
    } else {
        _wifiManager.autoConnect(_apName);
    }
    
    _state = State::CONNECTING;
    _setupDone = true;
    
    LOG_I("WiFi connection initiated, AP name: %s", _apName);
}

void WiFiManagerFeature::loop() {
    // Process WiFiManager (needed for non-blocking mode)
    _wifiManager.process();
    
    // Check current connection state
    bool currentlyConnected = (WiFi.status() == WL_CONNECTED);
    
    switch (_state) {
        case State::IDLE:
            // Should not happen after setup
            break;
            
        case State::CONNECTING:
            if (currentlyConnected) {
                _connected = true;
                _state = State::CONNECTED;
                LOG_I("WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
            }
            break;
            
        case State::CONNECTED:
            if (!currentlyConnected) {
                _connected = false;
                _state = State::DISCONNECTED;
                _lastReconnectAttempt = millis();
                LOG_W("WiFi disconnected!");
            }
            break;
            
        case State::DISCONNECTED:
            // Try to reconnect periodically
            if (millis() - _lastReconnectAttempt >= RECONNECT_INTERVAL) {
                _lastReconnectAttempt = millis();
                LOG_I("Attempting WiFi reconnection...");
                WiFi.reconnect();
                _state = State::CONNECTING;
            }
            break;
    }
}

bool WiFiManagerFeature::isConnected() const {
    return _connected && (WiFi.status() == WL_CONNECTED);
}

String WiFiManagerFeature::getIPAddress() const {
    if (isConnected()) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}
