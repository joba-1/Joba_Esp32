#include <Arduino.h>
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "StorageFeature.h"
#include "InfluxDBFeature.h"
#include "MQTTFeature.h"
#include "LEDFeature.h"
#include "DataCollection.h"
#include "DataCollectionWeb.h"
#include "DataCollectionMQTT.h"
#include "ModbusRTUFeature.h"
#include "HardwareSerialAdapter.h"
#include "ModbusDevice.h"
#include "ModbusWeb.h"
#include "ModbusIntegration.h"
#include "MQTTIntegrationFeature.h"
#include "SensorCollectionFeature.h"
#include "ModbusDeviceFeature.h"
#include "ResetManager.h"
#include "ResetDiagnostics.h"
#include "CpuMonitor.h"
#include "main_helper.h"
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

// ============================================
// Example Data Collection Definition
// ============================================

static void markOtaAppValidIfPendingVerify() {
    /**
     * @brief If the running partition is in PENDING_VERIFY state, mark the
     *        application as valid to prevent OTA rollback.
     *
     * This is called early in `setup()` so that freshly-updated images are
     * preserved when the firmware's behavior is considered stable.
     */
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
// Wrap HardwareSerial in adapter for dependency inversion
static HardwareSerialAdapter serial2Adapter(Serial2);
ModbusRTUFeature modbus(
    serial2Adapter,
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

// Wrap ModbusDeviceManager as a Feature for CPU tracking
ModbusDeviceFeature modbusDeviceFeature(modbusDevices);

// Forward-declare sensor reading callback (defined below)
static void fillSensorReading(SensorData& reading);

// Sensor data collection feature (periodic collect + persistence loop)
SensorCollectionFeature<SensorData, 100> sensorCollectionFeature(
    sensorData, influxDB, mqtt, led,
    fillSensorReading, "sensors", 60000
);

// MQTT integration feature (HA discovery, cmd subscriptions, state publishing)
// Configured in setup() via configure() once dynamic values are available
MQTTIntegrationFeature mqttIntegration(
    mqtt,
    sensorHAConfig, sizeof(sensorHAConfig) / sizeof(sensorHAConfig[0]),
    30000
);

// Array of all features for easy iteration — order matters
Feature* features[] = {
    &logging,               // Must be first for early logging
    &led,                   // LED setup early to indicate boot
    &wifiManager,           // Must be before network-dependent features
    &timeSync,
    &storage,               // Filesystem before features that need it
    &webServer,
    &influxDB,
    &mqtt,                  // MQTT after network is ready
    &modbus,                // Modbus RTU bus monitor
    &modbusDeviceFeature,   // Modbus device polling
    &sensorCollectionFeature, // Periodic sensor collection + persistence
    &mqttIntegration        // HA discovery, MQTT subscriptions, state publish
};
const size_t featureCount = sizeof(features) / sizeof(features[0]);

// Callback for SensorCollectionFeature — fills a reading with sensor values
static void fillSensorReading(SensorData& reading) {
    /**
     * @brief Populate a single `SensorData` entry with current measurements.
     * @param reading Reference to a pre-allocated SensorData to fill.
     *
     * Currently this generates simulated values; replace with real sensor
     * acquisition as needed.
     */
    strncpy(reading.location, deviceId.c_str(), sizeof(reading.location) - 1);
    reading.temperature = 22.5 + (random(-20, 20) / 10.0f);  // Simulated
    reading.humidity = 55.0 + (random(-100, 100) / 10.0f);   // Simulated
    reading.rssi = WiFi.RSSI();
    LOG_D("Collected: temp=%.1f, humidity=%.1f, rssi=%d",
          reading.temperature, reading.humidity, reading.rssi);
}

/**
 * @brief Main Arduino `setup()` routine.
 *
 * Initializes diagnostic counters, features (network, storage, web, MQTT,
 * Modbus, etc.), configures dynamic IDs and registers callbacks.
 */
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
    logging.setHostname(hostname.c_str());
    mqtt.setClientId(mqttClientId.c_str());
    mqtt.setBaseTopic(mqttBaseTopic.c_str());

    // MQTT reset command handler (armed only when MQTT is connected & subscribed)
    mqtt.onMessage([](const char* topic, const char* payload) {
        if (!topic || !payload) return;

        const String resetTopic = mqttBaseTopic + "/cmd/reset";
        const String restartTopic = mqttBaseTopic + "/cmd/restart";
        const String modbusRawReadTopic = mqttBaseTopic + "/modbus/cmd/raw/read";
        const String modbusRawWriteTopic = mqttBaseTopic + "/modbus/cmd/raw/write";
        const String modbusWriteTopic = mqttBaseTopic + "/modbus/cmd/write";
        const String modbusReadTopic = mqttBaseTopic + "/modbus/cmd/read";
        const String t(topic);
        // Only handle topics under our base topic
        if (!t.startsWith(mqttBaseTopic)) return;

        if (t == resetTopic || t == restartTopic) {
            MainHelper::handleResetCommand(payload, resetTopic, mqtt);
            return;
        }

        if (t == modbusRawReadTopic) {
            MainHelper::handleModbusRawReadCommand(payload, modbusRawReadTopic, mqtt, modbus);
            return;
        }

        if (t == modbusRawWriteTopic) {
            MainHelper::handleModbusRawWriteCommand(payload, modbusRawWriteTopic, mqtt, modbus);
            return;
        }

        if (t == modbusWriteTopic) {
            MainHelper::handleModbusWriteCommand(payload, modbusWriteTopic, mqtt, modbusDevices);
            return;
        }

        if (t == modbusReadTopic) {
            MainHelper::handleModbusReadCommand(payload, modbusReadTopic, mqtt, modbusDevices);
            return;
        }

        // List known Modbus devices (immediate response)
        const String modbusListDevicesTopic = mqttBaseTopic + "/modbus/cmd/list_devices";
        if (t == modbusListDevicesTopic) {
            MainHelper::handleModbusListDevicesCommand(mqtt, modbusDevices);
            return;
        }

        // List registers for a device by unit ID
        const String modbusListRegistersTopic = mqttBaseTopic + "/modbus/cmd/list_registers";
        if (t == modbusListRegistersTopic) {
            MainHelper::handleModbusListRegistersCommand(payload, mqtt, modbusDevices);
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
    
    // Configure MQTTIntegrationFeature with dynamic values now available
    mqttIntegration.configure(
        mqttBaseTopic.c_str(),
        deviceId.c_str(),
        modbusDevices
    );

    // Enable periodic CPU stats logging (every 60 seconds)
    CpuMonitor::setLogInterval(60000);

    // Setup complete - turn off LED (will pulse on activity)
    led.setupComplete();
    ResetDiagnostics::setBreadcrumb("setup", "done");
}

/**
 * @brief Main Arduino `loop()` routine.
 *
 * Iterates over all registered `Feature` instances and records per-feature
 * CPU usage via `ResetDiagnostics` and `CpuMonitor`.
 */
void loop() {
    CpuMonitor::markLoopStart();

    // Run all feature loop handlers — each gets automatic CPU timing
    for (size_t i = 0; i < featureCount; i++) {
        ResetDiagnostics::setBreadcrumb("loop", features[i]->getName());
        const uint32_t startUs = (uint32_t)micros();
        features[i]->loop();
        const uint32_t durUs = (uint32_t)((uint32_t)micros() - startUs);
        ResetDiagnostics::recordLoopDurationUs(features[i]->getName(), durUs);
    }

    CpuMonitor::markLoopEnd();

    // Small delay to allow WiFi/TCP stack and other background tasks to run.
    // Without this, the tight loop can starve RTOS background tasks.
    delay(2);
}
