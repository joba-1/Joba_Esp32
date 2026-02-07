#include "MQTTFeature.h"

MQTTFeature* MQTTFeature::_instance = nullptr;

MQTTFeature::MQTTFeature(const char* server, uint16_t port,
                         const char* username, const char* password,
                         const char* clientId, const char* baseTopic,
                         uint32_t reconnectIntervalMs)
    : _server(server)
    , _port(port)
    , _username(username)
    , _password(password)
    , _clientId(clientId)
    , _baseTopic(baseTopic)
    , _reconnectIntervalMs(reconnectIntervalMs)
    , _connected(false)
    , _lastReconnectAttempt(0)
    , _messageCallback(nullptr)
{
    _instance = this;
    _mqttClient.setClient(_wifiClient);
}

void MQTTFeature::setup() {
    if (strlen(_server) == 0) {
        LOG_I("MQTT disabled (no server configured)");
        return;
    }
    
    _mqttClient.setServer(_server, _port);
    _mqttClient.setCallback(mqttCallback);
    _mqttClient.setBufferSize(1024);  // Larger buffer for HA discovery
    
    LOG_I("MQTT configured for %s:%d", _server, _port);
}

void MQTTFeature::loop() {
    if (strlen(_server) == 0) return;
    if (!WiFi.isConnected()) return;
    
    if (!_mqttClient.connected()) {
        _connected = false;
        unsigned long now = millis();
        if (now - _lastReconnectAttempt >= _reconnectIntervalMs) {
            _lastReconnectAttempt = now;
            reconnect();
        }
    } else {
        _mqttClient.loop();
    }
}

void MQTTFeature::reconnect() {
    LOG_D("Attempting MQTT connection to %s...", _server);
    
    bool connected;
    if (strlen(_username) > 0) {
        connected = _mqttClient.connect(_clientId, _username, _password);
    } else {
        connected = _mqttClient.connect(_clientId);
    }
    
    if (connected) {
        _connected = true;
        LOG_I("MQTT connected as %s", _clientId);
        
        // Publish online status
        String statusTopic = String(_baseTopic) + "/status";
        _mqttClient.publish(statusTopic.c_str(), "online", true);
    } else {
        LOG_W("MQTT connection failed, rc=%d", _mqttClient.state());
    }
}

bool MQTTFeature::publish(const char* topic, const char* payload, bool retain) {
    if (!_connected) return false;
    return _mqttClient.publish(topic, payload, retain);
}

bool MQTTFeature::publishToBase(const char* subtopic, const char* payload, bool retain) {
    // Use instance buffer instead of heap allocation
    snprintf(_topicBuffer, MAX_TOPIC_LEN, "%s/%s", _baseTopic, subtopic);
    return publish(_topicBuffer, payload, retain);
}

bool MQTTFeature::subscribe(const char* topic) {
    if (!_connected) return false;
    return _mqttClient.subscribe(topic);
}

bool MQTTFeature::subscribeToBase(const char* subtopic) {
    // Use instance buffer instead of heap allocation
    snprintf(_topicBuffer, MAX_TOPIC_LEN, "%s/%s", _baseTopic, subtopic);
    return subscribe(_topicBuffer);
}

void MQTTFeature::onMessage(MessageCallback callback) {
    _messageCallback = callback;
}

void MQTTFeature::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance && _instance->_messageCallback) {
        // Null-terminate the payload
        char* msg = new char[length + 1];
        memcpy(msg, payload, length);
        msg[length] = '\0';
        
        _instance->_messageCallback(topic, msg);
        
        delete[] msg;
    }
}
