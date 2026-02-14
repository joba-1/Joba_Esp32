#ifndef MQTT_FEATURE_H
#define MQTT_FEATURE_H

#include "Feature.h"
#include "LoggingFeature.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

/**
 * @brief Lightweight MQTT client feature
 *
 * Wraps `PubSubClient` and provides auto-reconnect, topic helpers that
 * publish under a configured base topic, and a simple callback interface
 * for incoming messages. Designed for use as a firmware `Feature`.
 */
class MQTTFeature : public Feature {
public:
    /**
     * @brief Callback type for incoming MQTT messages
     * @param topic Topic string (NUL-terminated)
     * @param payload Payload string (NUL-terminated)
     */
    using MessageCallback = std::function<void(const char* topic, const char* payload)>;

    /**
     * @brief Construct the MQTT feature
     * @param server MQTT broker hostname or IP
     * @param port MQTT broker port
     * @param username Optional username for broker authentication
     * @param password Optional password for broker authentication
     * @param clientId MQTT client id used for connect
     * @param baseTopic Base topic used by `publishToBase`/`subscribeToBase`
     * @param reconnectIntervalMs Milliseconds between reconnect attempts
     */
    MQTTFeature(const char* server, uint16_t port, 
                const char* username, const char* password,
                const char* clientId, const char* baseTopic,
                uint32_t reconnectIntervalMs = 5000);
    
    /**
     * @brief Initialize MQTT client and reconnect state
     *
     * This prepares the underlying `PubSubClient` instance and does not
     * block while attempting connections. Actual reconnect attempts are
     * driven from `loop()`.
     */
    void setup() override;

    /**
     * @brief Periodic work: reconnect and handle incoming messages
     *
     * Should be called frequently from the main firmware loop. This method
     * handles connection retries using `_reconnectIntervalMs` and pumps the
     * `PubSubClient` loop when connected.
     */
    void loop() override;
    const char* getName() const override { return "MQTT"; }
    bool isReady() const override { return _connected; }
    
    /** Publishing helpers */
    bool publish(const char* topic, const char* payload, bool retain = false);
    bool publishToBase(const char* subtopic, const char* payload, bool retain = false);
    // Publish large payloads by splitting into multiple parts when needed
    bool publishLarge(const char* subtopic, const char* payload, bool retain = false);
    
    /** Subscription helpers */
    bool subscribe(const char* topic);
    bool subscribeToBase(const char* subtopic);
    void onMessage(MessageCallback callback);
    
    /** Connection info */
    bool isConnected() const { return _connected; }
    const char* getBaseTopic() const { return _baseTopic; }
    const char* getClientId() const { return _clientId; }
    
    /** Setters for runtime configuration */
    void setClientId(const char* clientId) { _clientId = clientId; }
    void setBaseTopic(const char* baseTopic) { _baseTopic = baseTopic; }
    
    /** Singleton access (if used) */
    static MQTTFeature* getInstance() { return _instance; }

private:
    static MQTTFeature* _instance;
    
    WiFiClient _wifiClient;           /**< underlying WiFi TCP client */
    PubSubClient _mqttClient;         /**< PubSubClient wrapper */
    
    const char* _server;              /**< broker host */
    uint16_t _port;
    const char* _username;            /**< optional broker username */
    const char* _password;            /**< optional broker password */
    const char* _clientId;            /**< MQTT client identifier */
    const char* _baseTopic;           /**< configured base topic for publishes/subscribes */
    uint32_t _reconnectIntervalMs;    /**< reconnect backoff interval */
    
    bool _connected;                  /**< connection state */
    unsigned long _lastReconnectAttempt; /**< last attempt time (millis) */
    
    MessageCallback _messageCallback; /**< user-supplied message handler */
    
    // Static buffer for topic building (avoids heap allocation per publish)
    static constexpr size_t MAX_TOPIC_LEN = 128;
    char _topicBuffer[MAX_TOPIC_LEN];
    
    /**
     * @brief Attempt to (re)connect to the MQTT broker
     *
     * This performs a non-blocking connect sequence using the configured
     * `_username` / `_password` (if present) and sets subscriptions after a
     * successful connect. Called from `loop()` when not connected and the
     * reconnect interval has elapsed.
     */
    void reconnect();

    /**
     * @brief Static callback passed to `PubSubClient`
     *
     * Converts the raw `payload` bytes into a NUL-terminated string and
     * forwards the call to the instance `_messageCallback` if present.
     */
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
};

#endif // MQTT_FEATURE_H
