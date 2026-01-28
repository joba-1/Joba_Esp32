#ifndef MQTT_FEATURE_H
#define MQTT_FEATURE_H

#include "Feature.h"
#include "LoggingFeature.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

/**
 * @brief MQTT client feature with auto-reconnect
 */
class MQTTFeature : public Feature {
public:
    using MessageCallback = std::function<void(const char* topic, const char* payload)>;

    MQTTFeature(const char* server, uint16_t port, 
                const char* username, const char* password,
                const char* clientId, const char* baseTopic,
                uint32_t reconnectIntervalMs = 5000);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "MQTT"; }
    bool isReady() const override { return _connected; }
    
    // Publishing
    bool publish(const char* topic, const char* payload, bool retain = false);
    bool publishToBase(const char* subtopic, const char* payload, bool retain = false);
    
    // Subscribing
    bool subscribe(const char* topic);
    bool subscribeToBase(const char* subtopic);
    void onMessage(MessageCallback callback);
    
    // Connection info
    bool isConnected() const { return _connected; }
    const char* getBaseTopic() const { return _baseTopic; }
    const char* getClientId() const { return _clientId; }
    
    // Setter for dynamic configuration
    void setClientId(const char* clientId) { _clientId = clientId; }
    void setBaseTopic(const char* baseTopic) { _baseTopic = baseTopic; }
    
    // Singleton access
    static MQTTFeature* getInstance() { return _instance; }

private:
    static MQTTFeature* _instance;
    
    WiFiClient _wifiClient;
    PubSubClient _mqttClient;
    
    const char* _server;
    uint16_t _port;
    const char* _username;
    const char* _password;
    const char* _clientId;
    const char* _baseTopic;
    uint32_t _reconnectIntervalMs;
    
    bool _connected;
    unsigned long _lastReconnectAttempt;
    
    MessageCallback _messageCallback;
    
    void reconnect();
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
};

#endif // MQTT_FEATURE_H
