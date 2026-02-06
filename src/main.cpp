#include <Arduino.h>
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "OTAFeature.h"
#include "StorageFeature.h"
#include "InfluxDBFeature.h"
#include "MQTTFeature.h"
#include "LEDFeature.h"
#include "DataCollection.h"
#include "DataCollectionWeb.h"
#include "DataCollectionMQTT.h"
#include "ModbusRTUFeature.h"
#include "ModbusDevice.h"
#include "ModbusWeb.h"
#include "ModbusIntegration.h"
#include "ResetManager.h"
#include "ResetDiagnostics.h"
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

// ============================================
// Example Data Collection Definition
// ============================================

static void markOtaAppValidIfPendingVerify() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        // If OTA rollback is enabled, newly-booted images may start in a
        // "pending verify" state and will be rolled back unless marked valid.
        // Mark as valid immediately; this firmware is expected to be stable.
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

// Define your data structure
struct SensorData {
    uint32_t timestamp;     // Unix timestamp (auto-filled)
    char location[16];      // InfluxDB tag
    float temperature;      // InfluxDB field
    float humidity;         // InfluxDB field
    int32_t rssi;          // WiFi signal strength
};

// Define the schema for serialization
const FieldDescriptor SensorDataSchema[] = {
    FIELD_UINT32(SensorData, timestamp,   TIMESTAMP),
    FIELD_STRING(SensorData, location,    TAG),
    FIELD_FLOAT(SensorData, temperature,  FIELD),
    FIELD_FLOAT(SensorData, humidity,     FIELD),
    FIELD_INT32(SensorData, rssi,         FIELD),
};
const size_t SensorDataSchemaSize = sizeof(SensorDataSchema) / sizeof(SensorDataSchema[0]);

// Create data collection with 100-entry RAM buffer
DataCollection<SensorData, 100> sensorData(
    "sensors",
    SensorDataSchema,
    SensorDataSchemaSize,
    "environment"  // InfluxDB measurement name
);

// ============================================
// Feature Instances
// ============================================

// Dynamic device identity (computed at runtime)
String deviceId;
String hostname;
String apName;
String mqttClientId;
String mqttBaseTopic;
String defaultPassword;

// Feature instances - parameters come from platformio.ini build_flags
LoggingFeature logging(
    LOG_BAUD_RATE,
    LOG_SERIAL_BOOT_LEVEL,
    LOG_SERIAL_RUNTIME_LEVEL,
    LOG_BOOT_DURATION_MS,
    LOG_SYSLOG_LEVEL,
    LOG_SYSLOG_SERVER,
    LOG_SYSLOG_PORT,
    "",  // Hostname set dynamically in setup()
    LOG_ENABLE_TIMESTAMP
);
WiFiManagerFeature wifiManager("", "", WIFI_CONFIG_PORTAL_TIMEOUT);  // AP name and password set in setup()
TimeSyncFeature timeSync(NTP_SERVER1, NTP_SERVER2, TIMEZONE, NTP_SYNC_INTERVAL);
StorageFeature storage(true);  // Format on fail
WebServerFeature webServer(WEBSERVER_PORT, WEBSERVER_USERNAME, "");  // Password set in setup()
OTAFeature ota("", "", OTA_PORT);  // Hostname and password set in setup()

// InfluxDB: supports V1.x (user/password) and V2.x (org/bucket/token)
// Note: Database name defaults to FIRMWARE_NAME if empty
#if INFLUXDB_VERSION == 2
InfluxDBFeature influxDB(
    INFLUXDB_URL,
    INFLUXDB_ORG,
    strlen(INFLUXDB_BUCKET) > 0 ? INFLUXDB_BUCKET : FIRMWARE_NAME,
    INFLUXDB_TOKEN,
    INFLUXDB_BATCH_INTERVAL,
    INFLUXDB_BATCH_SIZE
);
#else
// Default to V1.x
InfluxDBFeature influxDB = InfluxDBFeature::createV1(
    INFLUXDB_URL,
    strlen(INFLUXDB_DATABASE) > 0 ? INFLUXDB_DATABASE : FIRMWARE_NAME,
    INFLUXDB_USERNAME,
    INFLUXDB_PASSWORD,
    INFLUXDB_RP,
    INFLUXDB_BATCH_INTERVAL,
    INFLUXDB_BATCH_SIZE
);
#endif

MQTTFeature mqtt(
    MQTT_SERVER,
    MQTT_PORT,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    "",  // Client ID set in setup()
    "",  // Base topic set in setup()
    MQTT_RECONNECT_INTERVAL
);

// Modbus RTU feature - monitors bus and handles requests
ModbusRTUFeature modbus(
    Serial2,                    // Hardware serial port
    MODBUS_BAUD_RATE,
    MODBUS_SERIAL_CONFIG,
    MODBUS_SERIAL_RX,
    MODBUS_SERIAL_TX,
    MODBUS_DE_PIN,              // RS485 DE pin (-1 if not used)
    MODBUS_QUEUE_SIZE,
    MODBUS_RESPONSE_TIMEOUT
);

// LED indicator feature
LEDFeature led(LED_PIN, LED_ACTIVE_LOW, LED_PULSE_DURATION);

// Home Assistant sensor configuration for our data collection
const HASensorConfig sensorHAConfig[] = {
    { "temperature", "Temperature", HADeviceClass::TEMPERATURE, "°C", nullptr },
    { "humidity", "Humidity", HADeviceClass::HUMIDITY, "%", nullptr },
    { "rssi", "WiFi Signal", HADeviceClass::SIGNAL_STRENGTH, "dBm", nullptr },
};

// Modbus device manager (declared here, initialized after storage is ready)
ModbusDeviceManager* modbusDevices = nullptr;

// Array of all features for easy iteration
Feature* features[] = {
    &logging,      // Must be first for early logging
    &led,          // LED setup early to indicate boot
    &wifiManager,  // Must be before network-dependent features
    &timeSync,
    &storage,      // Filesystem before features that need it
    &webServer,
    &ota,
    &influxDB,
    &mqtt,         // MQTT after network is ready
    &modbus        // Modbus RTU bus monitor
};
const size_t featureCount = sizeof(features) / sizeof(features[0]);

// ============================================
// Data Collection Timer
// ============================================
unsigned long lastDataCollection = 0;
const unsigned long DATA_COLLECTION_INTERVAL = 60000;  // Collect every 60 seconds
bool haDiscoveryPublished = false;
bool modbusHADiscoveryPublished = false;

// MQTT command subscriptions are lost on reconnect; track and re-subscribe.
bool mqttResetCmdSubscribed = false;
unsigned long lastModbusStatePublish = 0;
const unsigned long MODBUS_STATE_PUBLISH_INTERVAL = 30000;  // Publish state every 30s

void collectSensorData() {
    SensorData reading;
    memset(&reading, 0, sizeof(reading));
    
    // Set location tag using device ID
    strncpy(reading.location, deviceId.c_str(), sizeof(reading.location) - 1);
    
    // Collect sensor values (replace with actual sensor readings)
    reading.temperature = 22.5 + (random(-20, 20) / 10.0f);  // Simulated
    reading.humidity = 55.0 + (random(-100, 100) / 10.0f);   // Simulated
    reading.rssi = WiFi.RSSI();
    
    // Add to collection (timestamp auto-filled)
    sensorData.add(reading);
    
    // Queue for InfluxDB upload
    influxDB.queue(sensorData.latestToLineProtocol());
    
    // Publish to MQTT for Home Assistant
    DataCollectionMQTT::publishLatest(&mqtt, sensorData, "sensors");
    
    // Pulse LED to indicate data collection
    led.pulse();
    
    LOG_D("Collected: temp=%.1f, humidity=%.1f, rssi=%d", 
          reading.temperature, reading.humidity, reading.rssi);
}

void setup() {
    // Capture reset reason + boot counter early for diagnostics.
    ResetDiagnostics::init();
    ResetDiagnostics::setBreadcrumb("setup", "start");

    // Ensure OTA updates stick even when rollback is enabled.
    // Must run early, before any long initialization.
    markOtaAppValidIfPendingVerify();

    // Initialize WiFi in station mode (needed for MAC address)
    WiFi.mode(WIFI_STA);
    
    // Generate device identity
    deviceId = DeviceInfo::getDeviceId();
    hostname = DeviceInfo::getHostname();
    
    WiFi.mode(WIFI_OFF);

    // Set WiFi hostname BEFORE connecting (required for proper mDNS registration)
    WiFi.setHostname(hostname.c_str());
    
    WiFi.mode(WIFI_STA);

    apName = deviceId + "-Config";
    mqttClientId = hostname;
    mqttBaseTopic = String(DeviceInfo::getFirmwareName()) + "/" + hostname;
    mqttBaseTopic.toLowerCase();
    defaultPassword = DeviceInfo::getDefaultPassword(DEFAULT_PASSWORD);
    
    // Configure features with dynamic values before setup
    wifiManager.setAPName(apName.c_str());
    wifiManager.setAPPassword(defaultPassword.c_str());
    webServer.setPassword(defaultPassword.c_str());
    ota.setHostname(hostname.c_str());
    ota.setPassword(defaultPassword.c_str());
    logging.setHostname(hostname.c_str());
    mqtt.setClientId(mqttClientId.c_str());
    mqtt.setBaseTopic(mqttBaseTopic.c_str());

    // MQTT reset command handler (armed only when MQTT is connected & subscribed)
    mqtt.onMessage([](const char* topic, const char* payload) {
        if (!topic || !payload) return;

        const String resetTopic = mqttBaseTopic + "/cmd/reset";
        const String restartTopic = mqttBaseTopic + "/cmd/restart";
        const String modbusRawReadTopic = mqttBaseTopic + "/modbus/cmd/raw/read";
        const String t(topic);
        if (t != resetTopic && t != restartTopic && t != modbusRawReadTopic) return;

        if (t == resetTopic || t == restartTopic) {
            String p(payload);
            p.trim();
            p.toLowerCase();
            if (p != "1" && p != "true" && p != "reset" && p != "restart" && p != "reboot") {
                LOG_W("MQTT reset ignored (payload='%s')", payload);
                return;
            }

            const bool scheduled = ResetManager::scheduleRestart(250, "mqtt");
            mqtt.publishToBase("status/reset", scheduled ? "scheduled" : "already_scheduled", false);
            return;
        }

        // Modbus raw read command
        // Topic: <base>/modbus/cmd/raw/read
        // Payload JSON: {"id":"optional", "unit":1, "address":0, "count":2, "fc":3}
        if (t == modbusRawReadTopic) {
#if MODBUS_LISTEN_ONLY
            mqtt.publishToBase("modbus/ack/raw/read", "{\"queued\":false,\"error\":\"listen_only\"}", false);
            return;
#else
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);

            String id = doc["id"].is<const char*>() ? String((const char*)doc["id"]) : String((uint32_t)millis());
            uint8_t unitId = doc["unit"] | 0;
            uint16_t address = doc["address"] | 0;
            uint16_t count = doc["count"] | 0;
            uint8_t fc = doc["fc"] | 3;

            JsonDocument ack;
            ack["id"] = id;
            ack["topic"] = (const char*)modbusRawReadTopic.c_str();

            if (err || unitId == 0 || count == 0) {
                ack["queued"] = false;
                ack["error"] = err ? "invalid_json" : "invalid_params";
                String out;
                serializeJson(ack, out);
                mqtt.publishToBase("modbus/ack/raw/read", out.c_str(), false);
                return;
            }

            // ACK immediately so callers know we accepted the command.
            ack["queued"] = true;
            ack["unitId"] = unitId;
            ack["address"] = address;
            ack["count"] = count;
            ack["functionCode"] = fc;
            String outAck;
            serializeJson(ack, outAck);
            mqtt.publishToBase("modbus/ack/raw/read", outAck.c_str(), false);

            bool queued = modbus.queueReadRegisters(
                unitId, fc, address, count,
                [id](bool success, const ModbusFrame& response) {
                    JsonDocument resp;
                    resp["id"] = id;
                    resp["unitId"] = response.unitId;
                    resp["functionCode"] = response.functionCode;
                    resp["success"] = success;
                    resp["isException"] = response.isException;
                    if (response.isException) resp["exceptionCode"] = response.exceptionCode;
                    {
                        char crcHex[7];
                        snprintf(crcHex, sizeof(crcHex), "0x%04X", (unsigned)response.crc);
                        resp["crcHex"] = crcHex;
                    }

                    // Raw payload hex (no unit/fc/crc)
                    resp["dataHex"] = modbus.formatHex(response.data.data(), response.data.size());

                    uint8_t fcBase = response.functionCode & 0x7F;
                    if (!response.isException && (fcBase == ModbusFC::READ_HOLDING_REGISTERS || fcBase == ModbusFC::READ_INPUT_REGISTERS)) {
                        const size_t byteCount = response.getByteCount();
                        const uint8_t* regData = response.getRegisterData();
                        resp["byteCount"] = (uint32_t)byteCount;
                        if (regData && byteCount >= 2) {
                            resp["registerDataHex"] = modbus.formatHex(regData, byteCount);

                            JsonArray words = resp["registerWords"].to<JsonArray>();
                            size_t wordCount = byteCount / 2;
                            static constexpr size_t MAX_WORDS = 32;
                            size_t emitCount = wordCount > MAX_WORDS ? MAX_WORDS : wordCount;
                            for (size_t i = 0; i < emitCount; i++) {
                                size_t idx = i * 2;
                                uint16_t w = ((uint16_t)regData[idx] << 8) | (uint16_t)regData[idx + 1];
                                words.add(w);
                            }
                            if (wordCount > MAX_WORDS) {
                                resp["registerWordsTruncated"] = true;
                                resp["registerWordCount"] = (uint32_t)wordCount;
                            }
                        }
                    }

                    resp["uptimeMs"] = (uint32_t)millis();
                    String out;
                    serializeJson(resp, out);
                    mqtt.publishToBase("modbus/resp/raw/read", out.c_str(), false);
                });

            if (!queued) {
                JsonDocument nack;
                nack["id"] = id;
                nack["queued"] = false;
                nack["error"] = "queue_failed";
                String out;
                serializeJson(nack, out);
                mqtt.publishToBase("modbus/ack/raw/read", out.c_str(), false);
            }
#endif
            return;
        }
    });
    
    // Log firmware info
    LOG_I("======================================");
    LOG_I("%s v%s", DeviceInfo::getFirmwareName(), DeviceInfo::getFirmwareVersion());
    LOG_I("Device ID: %s", deviceId.c_str());
    LOG_I("Hostname: %s", hostname.c_str());
    LOG_I("Default Password: %s", defaultPassword.c_str());
    LOG_I("Boot Count (RTC): %u", (unsigned)ResetDiagnostics::bootCount());
    LOG_I("Reset Reason: %s (%d)", ResetDiagnostics::resetReasonString(), (int)ResetDiagnostics::resetReason());
    LOG_I("RTC Reset Reason Core0/Core1: %u/%u", (unsigned)ResetDiagnostics::rtcResetReasonCore0(), (unsigned)ResetDiagnostics::rtcResetReasonCore1());
    LOG_I("======================================");
    
    // Initialize all features
    for (size_t i = 0; i < featureCount; i++) {
        ResetDiagnostics::setBreadcrumb("setup", features[i]->getName());
        features[i]->setup();
        LOG_I("Feature '%s' setup complete", features[i]->getName());
    }
    
    // Set device ID on data collections for InfluxDB tags
    sensorData.setDeviceId(deviceId);
    
    // Enable persistence for sensor data (5 second delay before write)
    // sensorData.enablePersistence(&storage, "/data/sensors.json", 5000);
    
    // Load any previously saved data
    if (storage.isReady()) {
        String json = storage.readFile("/data/sensors.json");
        if (json.length() > 0) {
            sensorData.fromJson(json);
            LOG_I("Loaded %u sensor readings from storage", sensorData.count());
        }
    }
    
    // Register web endpoints for sensor data collection
    // Creates: /api/sensors (JSON all), /api/sensors/latest (JSON latest), /view/sensors (HTML table)
    DataCollectionWeb::registerCollection(
        webServer,
        sensorData,
        "sensors",
        5000  // Refresh every 5 seconds
    );
    
    // Initialize Modbus device manager with device definitions from filesystem
    modbusDevices = new ModbusDeviceManager(modbus, storage);
    if (storage.isReady()) {
        LOG_I("Free heap before Modbus init: %d bytes", ESP.getFreeHeap());
        
        // Load device type definitions from /modbus/devices/*.json
        modbusDevices->loadAllDeviceTypes(MODBUS_DEVICE_TYPES_PATH);
        LOG_I("Free heap after loading device types: %d bytes", ESP.getFreeHeap());
        
        // Load unit ID to device type mapping
        modbusDevices->loadDeviceMappings(MODBUS_DEVICE_MAP_PATH);
        LOG_I("Free heap after loading device mappings: %d bytes", ESP.getFreeHeap());
        
        LOG_I("Modbus devices loaded: %d device types, %d mapped units",
              modbusDevices->getDeviceTypeNames().size(),
              modbusDevices->getDevices().size());
        
        // Register callback for Modbus value changes -> InfluxDB + MQTT
        modbusDevices->onValueChange([](uint8_t unitId, const char* deviceName,
                                        const char* registerName, float value,
                                        const char* unit) {
            // Queue to InfluxDB
            ModbusIntegration::queueValueToInfluxDB(&influxDB, unitId, deviceName,
                                                     registerName, value, unit, "modbus");
            
            // Publish individual value to MQTT
            String modbusTopic = mqttBaseTopic + "/modbus";
            ModbusIntegration::publishRegisterValue(&mqtt, unitId, deviceName,
                                                     registerName, value, modbusTopic.c_str(), true /* retain */);
            
            // Pulse LED to indicate Modbus data received
            led.pulse();
            
            LOG_V("Modbus value: %s/%s = %.4f %s", deviceName, registerName, value, unit);
        });
    }
    
    // Register Modbus web endpoints
    ModbusWeb::setup(webServer, modbus, *modbusDevices);
    
    LOG_I("All features initialized");
    LOG_I("Free heap: %d bytes", ESP.getFreeHeap());
    
    // Setup complete - turn off LED (will pulse on activity)
    led.setupComplete();
    ResetDiagnostics::setBreadcrumb("setup", "done");
}

void loop() {
    // Run all feature loop handlers
    for (size_t i = 0; i < featureCount; i++) {
        ResetDiagnostics::setBreadcrumb("loop", features[i]->getName());
        const uint32_t startUs = (uint32_t)micros();
        features[i]->loop();
        const uint32_t durUs = (uint32_t)((uint32_t)micros() - startUs);
        ResetDiagnostics::recordLoopDurationUs(features[i]->getName(), durUs);
    }
    
    // Publish Home Assistant autodiscovery once MQTT is connected
    if (mqtt.isConnected() && !haDiscoveryPublished) {
        ResetDiagnostics::setBreadcrumb("job", "haDiscovery");
        String deviceName = String(DeviceInfo::getFirmwareName()) + " " + deviceId;
        DataCollectionMQTT::publishDiscovery(
            &mqtt,
            "sensors",
            sensorHAConfig,
            sizeof(sensorHAConfig) / sizeof(sensorHAConfig[0]),
            deviceName.c_str(),                   // Device name in HA
            deviceId.c_str(),                     // Device unique ID
            "joba-1",                             // Manufacturer
            DeviceInfo::getFirmwareName(),        // Model
            DeviceInfo::getFirmwareVersion()      // Software version
        );
        haDiscoveryPublished = true;
        LOG_I("Home Assistant autodiscovery published");
    }

    // Subscribe to MQTT reset commands after connect (and after reconnect)
    if (mqtt.isConnected()) {
        if (!mqttResetCmdSubscribed) {
            ResetDiagnostics::setBreadcrumb("job", "mqttSubscribeCmd");
            bool ok1 = mqtt.subscribeToBase("cmd/reset");
            bool ok2 = mqtt.subscribeToBase("cmd/restart");
            bool ok3 = mqtt.subscribeToBase("modbus/cmd/raw/read");
            mqttResetCmdSubscribed = (ok1 && ok2 && ok3);
            LOG_I("MQTT reset cmd subscribed: %s", mqttResetCmdSubscribed ? "yes" : "no");
        }
    } else {
        mqttResetCmdSubscribed = false;
    }
    
    // Publish Modbus Home Assistant autodiscovery once MQTT is connected
    if (mqtt.isConnected() && !modbusHADiscoveryPublished && modbusDevices) {
        ResetDiagnostics::setBreadcrumb("job", "modbusHADiscovery");
        String modbusTopic = mqttBaseTopic + "/modbus";
        ModbusIntegration::publishDiscovery(
            &mqtt,
            *modbusDevices,
            modbusTopic.c_str(),
            "joba-1",              // Manufacturer
            DeviceInfo::getFirmwareName(), // Model
            DeviceInfo::getFirmwareVersion() // Software version
        );
        modbusHADiscoveryPublished = true;
        LOG_I("Modbus Home Assistant autodiscovery published");
    }
    
    // Periodic Modbus state publishing to MQTT
    if (mqtt.isConnected() && modbusDevices &&
        millis() - lastModbusStatePublish >= MODBUS_STATE_PUBLISH_INTERVAL) {
        lastModbusStatePublish = millis();
        ResetDiagnostics::setBreadcrumb("job", "modbusStatePublish");
        String modbusTopic = mqttBaseTopic + "/modbus";
        ModbusIntegration::publishAllDeviceStates(&mqtt, *modbusDevices,
                                                   modbusTopic.c_str());
    }
    
    // Periodic data collection
    if (millis() - lastDataCollection >= DATA_COLLECTION_INTERVAL) {
        lastDataCollection = millis();
        ResetDiagnostics::setBreadcrumb("job", "collectSensorData");
        collectSensorData();
    }
    
    // Handle data collection persistence
    ResetDiagnostics::setBreadcrumb("loop", "sensorData");
    {
        const uint32_t startUs = (uint32_t)micros();
    sensorData.loop();
        const uint32_t durUs = (uint32_t)((uint32_t)micros() - startUs);
        ResetDiagnostics::recordLoopDurationUs("sensorData", durUs);
    }
    
    // Run Modbus device polling
    if (modbusDevices) {
        ResetDiagnostics::setBreadcrumb("loop", "modbusDevices");
        const uint32_t startUs = (uint32_t)micros();
        modbusDevices->loop();
        const uint32_t durUs = (uint32_t)((uint32_t)micros() - startUs);
        ResetDiagnostics::recordLoopDurationUs("modbusDevices", durUs);
    }
}
