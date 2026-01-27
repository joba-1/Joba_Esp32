#ifndef DATA_COLLECTION_MQTT_H
#define DATA_COLLECTION_MQTT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "DataCollection.h"
#include "MQTTFeature.h"

/**
 * @brief Home Assistant device classes for sensors
 */
namespace HADeviceClass {
    constexpr const char* TEMPERATURE = "temperature";
    constexpr const char* HUMIDITY = "humidity";
    constexpr const char* PRESSURE = "pressure";
    constexpr const char* BATTERY = "battery";
    constexpr const char* VOLTAGE = "voltage";
    constexpr const char* CURRENT = "current";
    constexpr const char* POWER = "power";
    constexpr const char* ENERGY = "energy";
    constexpr const char* SIGNAL_STRENGTH = "signal_strength";
    constexpr const char* TIMESTAMP = "timestamp";
    constexpr const char* DURATION = "duration";
    constexpr const char* ILLUMINANCE = "illuminance";
    constexpr const char* CO2 = "carbon_dioxide";
    constexpr const char* PM25 = "pm25";
    constexpr const char* PM10 = "pm10";
    constexpr const char* NONE = nullptr;
}

/**
 * @brief Sensor field configuration for Home Assistant
 */
struct HASensorConfig {
    const char* fieldName;           // Field name in DataCollection
    const char* displayName;         // Human-readable name
    const char* deviceClass;         // HA device class (or nullptr)
    const char* unit;                // Unit of measurement
    const char* icon;                // MDI icon (optional, nullptr for default)
};

/**
 * @brief Helper to publish DataCollection data to MQTT with Home Assistant autodiscovery
 */
class DataCollectionMQTT {
public:
    /**
     * @brief Register a data collection with Home Assistant autodiscovery
     * 
     * @param mqtt MQTT feature instance
     * @param collectionName Name for topics (e.g., "sensors")
     * @param sensorConfigs Array of sensor configurations
     * @param configCount Number of sensor configurations
     * @param deviceName Device name shown in HA
     * @param deviceId Unique device identifier
     * @param manufacturer Device manufacturer
     * @param model Device model
     * @param swVersion Software version
     */
    static void publishDiscovery(
        MQTTFeature* mqtt,
        const char* collectionName,
        const HASensorConfig* sensorConfigs,
        size_t configCount,
        const char* deviceName,
        const char* deviceId,
        const char* manufacturer = "ESP32",
        const char* model = "ESP32 Sensor",
        const char* swVersion = "1.0.0"
    ) {
        if (!mqtt || !mqtt->isConnected()) return;
        
        String baseTopic = mqtt->getBaseTopic();
        String stateTopic = baseTopic + "/" + collectionName + "/state";
        String availTopic = baseTopic + "/status";
        
        for (size_t i = 0; i < configCount; i++) {
            const HASensorConfig& cfg = sensorConfigs[i];
            
            // Build unique_id: deviceId_collectionName_fieldName
            String uniqueId = String(deviceId) + "_" + collectionName + "_" + cfg.fieldName;
            
            // Discovery topic: homeassistant/sensor/<deviceId>/<uniqueId>/config
            String discoveryTopic = "homeassistant/sensor/" + String(deviceId) + "/" + uniqueId + "/config";
            
            // Build config payload
            JsonDocument doc;
            doc["name"] = cfg.displayName;
            doc["unique_id"] = uniqueId;
            doc["state_topic"] = stateTopic;
            doc["value_template"] = String("{{ value_json.") + cfg.fieldName + " }}";
            doc["availability_topic"] = availTopic;
            doc["payload_available"] = "online";
            doc["payload_not_available"] = "offline";
            
            if (cfg.deviceClass) {
                doc["device_class"] = cfg.deviceClass;
            }
            if (cfg.unit) {
                doc["unit_of_measurement"] = cfg.unit;
            }
            if (cfg.icon) {
                doc["icon"] = cfg.icon;
            }
            
            // Device info (same for all sensors)
            JsonObject device = doc["device"].to<JsonObject>();
            device["identifiers"][0] = deviceId;
            device["name"] = deviceName;
            device["manufacturer"] = manufacturer;
            device["model"] = model;
            device["sw_version"] = swVersion;
            
            // Serialize and publish
            String payload;
            serializeJson(doc, payload);
            
            mqtt->publish(discoveryTopic.c_str(), payload.c_str(), true);
            LOG_D("HA discovery: %s", discoveryTopic.c_str());
        }
    }
    
    /**
     * @brief Publish the latest data from a collection to MQTT
     */
    template<typename T, size_t N>
    static void publishLatest(
        MQTTFeature* mqtt,
        DataCollection<T, N>& collection,
        const char* collectionName
    ) {
        if (!mqtt || !mqtt->isConnected()) return;
        if (collection.isEmpty()) return;
        
        String stateTopic = String(mqtt->getBaseTopic()) + "/" + collectionName + "/state";
        String json = collection.toJson(collection.count() - 1);
        
        mqtt->publish(stateTopic.c_str(), json.c_str(), false);
        LOG_D("MQTT publish: %s", stateTopic.c_str());
    }
    
    /**
     * @brief Publish all data from a collection as JSON array
     */
    template<typename T, size_t N>
    static void publishAll(
        MQTTFeature* mqtt,
        DataCollection<T, N>& collection,
        const char* collectionName
    ) {
        if (!mqtt || !mqtt->isConnected()) return;
        
        String topic = String(mqtt->getBaseTopic()) + "/" + collectionName + "/history";
        String json = collection.toJson();
        
        mqtt->publish(topic.c_str(), json.c_str(), false);
    }
    
    /**
     * @brief Remove discovery config (call before changing config)
     */
    static void removeDiscovery(
        MQTTFeature* mqtt,
        const char* collectionName,
        const HASensorConfig* sensorConfigs,
        size_t configCount,
        const char* deviceId
    ) {
        if (!mqtt || !mqtt->isConnected()) return;
        
        for (size_t i = 0; i < configCount; i++) {
            String uniqueId = String(deviceId) + "_" + collectionName + "_" + sensorConfigs[i].fieldName;
            String discoveryTopic = "homeassistant/sensor/" + String(deviceId) + "/" + uniqueId + "/config";
            
            // Publish empty payload to remove
            mqtt->publish(discoveryTopic.c_str(), "", true);
        }
    }
};

#endif // DATA_COLLECTION_MQTT_H
