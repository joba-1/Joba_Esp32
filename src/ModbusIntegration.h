#ifndef MODBUS_INTEGRATION_H
#define MODBUS_INTEGRATION_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ModbusDevice.h"
#include "MQTTFeature.h"
#include "InfluxDBFeature.h"
#include "DataCollectionMQTT.h"
#include "LoggingFeature.h"
#include "InfluxLineProtocol.h"

/**
 * @brief Helper class to integrate Modbus devices with InfluxDB and MQTT/Home Assistant
 * 
 * Provides:
 * - Automatic Home Assistant autodiscovery for all Modbus registers
 * - MQTT state publishing for device values
 * - InfluxDB line protocol generation and queueing
 */
class ModbusIntegration {
public:
    /**
     * @brief Publish Home Assistant autodiscovery for all Modbus devices
     * 
     * Creates sensor entities for each register on each device.
     * 
     * @param mqtt MQTT feature instance
     * @param devices ModbusDeviceManager with loaded devices
     * @param baseTopic Base MQTT topic (e.g., "esp32/modbus")
     * @param manufacturer Device manufacturer string
     * @param model Device model string
     * @param swVersion Software version string
     */
    static void publishDiscovery(MQTTFeature* mqtt,
                                  const ModbusDeviceManager& devices,
                                  const char* baseTopic,
                                  const char* manufacturer = "joba-1",
                                  const char* model = "ESP32 Modbus Gateway",
                                  const char* swVersion = "1.0.0") {
        if (!mqtt || !mqtt->isConnected()) return;
        
        for (const auto& devKv : devices.getDevices()) {
            const auto& device = devKv.second;
            if (!device.deviceType) continue;
            
            // Create a unique device ID
            String deviceId = String(baseTopic);
            deviceId.replace("/", "_");
            deviceId += "_unit";
            deviceId += String(device.unitId);
            
            // Publish discovery for each register
            for (const auto& reg : device.deviceType->registers) {
                publishRegisterDiscovery(mqtt, device, reg, baseTopic,
                                         deviceId.c_str(), manufacturer, model, swVersion);
            }
            
            LOG_I("Published HA discovery for %s (unit %d): %d sensors",
                  device.deviceName.c_str(), device.unitId,
                  device.deviceType->registers.size());
        }
    }
    
    /**
     * @brief Publish current state for all registers of a device to MQTT
     * 
     * @param mqtt MQTT feature instance
     * @param device Device instance
     * @param baseTopic Base MQTT topic
     */
    static void publishDeviceState(MQTTFeature* mqtt,
                                    const ModbusDeviceInstance& device,
                                    const char* baseTopic) {
        if (!mqtt || !mqtt->isConnected()) return;

        // Publish as individual topics to keep payload small (PubSubClient buffer)
        // and to make Home Assistant integration robust.
        for (const auto& kv : device.currentValues) {
            if (!kv.second.valid) continue;
            publishRegisterValue(mqtt,
                                 device.unitId,
                                 device.deviceName.c_str(),
                                 kv.first.c_str(),
                                 kv.second.value,
                                 baseTopic,
                                 true /* retain */);
        }
    }
    
    /**
     * @brief Publish state for all Modbus devices
     * 
     * @param mqtt MQTT feature instance
     * @param devices ModbusDeviceManager with loaded devices
     * @param baseTopic Base MQTT topic
     */
    static void publishAllDeviceStates(MQTTFeature* mqtt,
                                        const ModbusDeviceManager& devices,
                                        const char* baseTopic) {
        for (const auto& kv : devices.getDevices()) {
            publishDeviceState(mqtt, kv.second, baseTopic);
        }
    }
    
    /**
     * @brief Publish a single register value to MQTT
     * 
     * @param mqtt MQTT feature instance
     * @param unitId Device unit ID
     * @param deviceName Device name
     * @param registerName Register name
     * @param value Register value
     * @param baseTopic Base MQTT topic
     */
    static void publishRegisterValue(MQTTFeature* mqtt,
                                      uint8_t unitId,
                                      const char* deviceName,
                                      const char* registerName,
                                      float value,
                                      const char* baseTopic,
                                      bool retain = false) {
        if (!mqtt || !mqtt->isConnected()) return;
        
        // Topic: baseTopic/unit_N/registerName
        String topic = baseTopic;
        topic += "/unit_";
        topic += String(unitId);
        topic += "/";
        topic += registerName;

        mqtt->publish(topic.c_str(), String(value, 4).c_str(), retain);
    }
    
    /**
     * @brief Queue all device values to InfluxDB
     * 
     * @param influx InfluxDB feature instance
     * @param devices ModbusDeviceManager with loaded devices
     * @param measurement InfluxDB measurement name
     */
    static void queueToInfluxDB(InfluxDBFeature* influx,
                                 const ModbusDeviceManager& devices,
                                 const char* measurement = "modbus") {
        if (!influx) return;
        
        auto lines = devices.allToLineProtocol(measurement);
        for (const auto& line : lines) {
            influx->queue(line);
        }
    }
    
    /**
     * @brief Queue a single register value to InfluxDB
     * 
     * @param influx InfluxDB feature instance
     * @param unitId Device unit ID
     * @param deviceName Device name
     * @param registerName Register name
     * @param value Register value
     * @param unit Unit string
     * @param measurement InfluxDB measurement name
     */
    static void queueValueToInfluxDB(InfluxDBFeature* influx,
                                      uint8_t unitId,
                                      const char* deviceName,
                                      const char* registerName,
                                      float value,
                                      const char* unit,
                                      const char* measurement = "modbus") {
        if (!influx) return;
        
        // Build line protocol
        String line = InfluxLineProtocol::escapeMeasurement(measurement);
        line += ",device=";
        line += InfluxLineProtocol::escapeTag(deviceName);
        line += ",unit_id=";
        line += String(unitId);
        line += ",register=";
        line += InfluxLineProtocol::escapeTag(registerName);
        if (unit && strlen(unit) > 0) {
            line += ",unit=";
            line += InfluxLineProtocol::escapeTag(unit);
        }
        line += " value=";
        line += String(value, 4);
        
        influx->queue(line);
    }
    
private:
    /**
     * @brief Publish Home Assistant discovery for a single register
     */
    static void publishRegisterDiscovery(MQTTFeature* mqtt,
                                          const ModbusDeviceInstance& device,
                                          const ModbusRegisterDef& reg,
                                          const char* baseTopic,
                                          const char* deviceId,
                                          const char* manufacturer,
                                          const char* model,
                                          const char* swVersion) {
        // Build unique ID for this sensor
        String uniqueId = deviceId;
        uniqueId += "_";
        uniqueId += reg.name;
        
        // Determine device class from unit
        const char* deviceClass = inferDeviceClass(reg.unit);
        const char* stateClass = inferStateClass(reg.name);
        
        // Build discovery topic
        String discoveryTopic = "homeassistant/sensor/";
        discoveryTopic += uniqueId;
        discoveryTopic += "/config";
        
        // Build state topic
        // PublishRegisterValue() publishes one topic per register:
        //   <baseTopic>/unit_<n>/<registerName>
        String stateTopic = baseTopic;
        stateTopic += "/unit_";
        stateTopic += String(device.unitId);
        stateTopic += "/";
        stateTopic += reg.name;
        
        // Build discovery payload
        JsonDocument doc;
        doc["name"] = String(device.deviceName) + " " + reg.name;
        doc["unique_id"] = uniqueId;
        doc["state_topic"] = stateTopic;

        // Register values are published as plain numeric payloads; no JSON template needed.
        
        if (deviceClass) {
            doc["device_class"] = deviceClass;
        }
        if (stateClass) {
            doc["state_class"] = stateClass;
        }
        if (strlen(reg.unit) > 0) {
            doc["unit_of_measurement"] = reg.unit;
        }
        
        // Availability
        // Use the gateway-wide availability topic. MQTTFeature publishes this as retained
        // (<mqttBaseTopic>/status). Using baseTopic (".../modbus") here would require a
        // separate ".../modbus/status" publisher and makes HA show the entities as unavailable.
        String availTopic = String(mqtt->getBaseTopic()) + "/status";
        doc["availability_topic"] = availTopic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        
        // Device info
        JsonObject devInfo = doc["device"].to<JsonObject>();
        devInfo["identifiers"].add(deviceId);
        devInfo["name"] = device.deviceName;
        devInfo["manufacturer"] = manufacturer;
        devInfo["model"] = String(model) + " - " + device.deviceTypeName;
        devInfo["sw_version"] = swVersion;
        
        String payload;
        serializeJson(doc, payload);
        
        mqtt->publish(discoveryTopic.c_str(), payload.c_str(), true);  // retained
    }
    
    /**
     * @brief Infer Home Assistant device class from unit string
     */
    static const char* inferDeviceClass(const char* unit) {
        if (!unit || strlen(unit) == 0) return nullptr;
        
        // Temperature
        if (strcmp(unit, "°C") == 0 || strcmp(unit, "C") == 0 ||
            strcmp(unit, "°F") == 0 || strcmp(unit, "F") == 0) {
            return "temperature";
        }
        // Voltage
        if (strcmp(unit, "V") == 0 || strcmp(unit, "mV") == 0) {
            return "voltage";
        }
        // Current
        if (strcmp(unit, "A") == 0 || strcmp(unit, "mA") == 0) {
            return "current";
        }
        // Power
        if (strcmp(unit, "W") == 0 || strcmp(unit, "kW") == 0 ||
            strcmp(unit, "MW") == 0) {
            return "power";
        }
        // Energy
        if (strcmp(unit, "Wh") == 0 || strcmp(unit, "kWh") == 0 ||
            strcmp(unit, "MWh") == 0) {
            return "energy";
        }
        // Frequency
        if (strcmp(unit, "Hz") == 0) {
            return "frequency";
        }
        // Power factor
        if (strcmp(unit, "PF") == 0) {
            return "power_factor";
        }
        // Percentage (battery, humidity)
        if (strcmp(unit, "%") == 0) {
            return nullptr;  // Could be many things, let HA infer
        }
        // Apparent power
        if (strcmp(unit, "VA") == 0 || strcmp(unit, "kVA") == 0) {
            return "apparent_power";
        }
        // Reactive power
        if (strcmp(unit, "VAr") == 0 || strcmp(unit, "kVAr") == 0 ||
            strcmp(unit, "var") == 0 || strcmp(unit, "kvar") == 0) {
            return "reactive_power";
        }
        
        return nullptr;
    }
    
    /**
     * @brief Infer Home Assistant state class from register name
     */
    static const char* inferStateClass(const char* name) {
        if (!name) return nullptr;
        
        // Energy registers are typically total increasing
        if (strstr(name, "Energy") || strstr(name, "Total") ||
            strstr(name, "Import") || strstr(name, "Export")) {
            return "total_increasing";
        }
        
        // Most other values are measurements
        return "measurement";
    }
};

#endif // MODBUS_INTEGRATION_H
