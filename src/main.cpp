#include <Arduino.h>
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
#include "ModbusRTU.h"
#include "ModbusDevice.h"
#include "ModbusWeb.h"
#include "ModbusIntegration.h"

// ============================================
// Example Data Collection Definition
// ============================================

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

// Feature instances - parameters come from platformio.ini build_flags
LoggingFeature logging(
    LOG_BAUD_RATE,
    LOG_SERIAL_BOOT_LEVEL,
    LOG_SERIAL_RUNTIME_LEVEL,
    LOG_BOOT_DURATION_MS,
    LOG_SYSLOG_LEVEL,
    LOG_SYSLOG_SERVER,
    LOG_SYSLOG_PORT,
    OTA_HOSTNAME,  // Reuse OTA hostname for syslog
    LOG_ENABLE_TIMESTAMP
);
WiFiManagerFeature wifiManager(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_CONFIG_PORTAL_TIMEOUT);
TimeSyncFeature timeSync(NTP_SERVER1, NTP_SERVER2, TIMEZONE, NTP_SYNC_INTERVAL);
StorageFeature storage(true);  // Format on fail
WebServerFeature webServer(WEBSERVER_PORT, WEBSERVER_USERNAME, WEBSERVER_PASSWORD);
OTAFeature ota(OTA_HOSTNAME, OTA_PASSWORD, OTA_PORT);

// InfluxDB: supports V1.x (user/password) and V2.x (org/bucket/token)
#if INFLUXDB_VERSION == 2
InfluxDBFeature influxDB(
    INFLUXDB_URL,
    INFLUXDB_ORG,
    INFLUXDB_BUCKET,
    INFLUXDB_TOKEN,
    INFLUXDB_BATCH_INTERVAL,
    INFLUXDB_BATCH_SIZE
);
#else
// Default to V1.x
InfluxDBFeature influxDB = InfluxDBFeature::createV1(
    INFLUXDB_URL,
    INFLUXDB_DATABASE,
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
    OTA_HOSTNAME,       // Use OTA hostname as client ID
    MQTT_BASE_TOPIC,
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
    MODBUS_RESPONSE_TIMEOUT,
    MODBUS_QUEUE_SIZE
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
unsigned long lastModbusStatePublish = 0;
const unsigned long MODBUS_STATE_PUBLISH_INTERVAL = 30000;  // Publish state every 30s

void collectSensorData() {
    SensorData reading;
    memset(&reading, 0, sizeof(reading));
    
    // Set location tag
    strncpy(reading.location, OTA_HOSTNAME, sizeof(reading.location) - 1);
    
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
    // Initialize all features
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->setup();
        LOG_I("Feature '%s' setup complete", features[i]->getName());
    }
    
    // Enable persistence for sensor data (5 second delay before write)
    sensorData.enablePersistence(&storage, "/data/sensors.json", 5000);
    
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
        webServer.getServer(),
        sensorData,
        "sensors",
        5000  // Refresh every 5 seconds
    );
    
    // Initialize Modbus device manager with device definitions from filesystem
    modbusDevices = new ModbusDeviceManager(modbus, storage);
    if (storage.isReady()) {
        // Load device type definitions from /modbus/devices/*.json
        modbusDevices->loadAllDeviceTypes(MODBUS_DEVICE_TYPES_PATH);
        
        // Load unit ID to device type mapping
        modbusDevices->loadDeviceMappings(MODBUS_DEVICE_MAP_PATH);
        
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
            ModbusIntegration::publishRegisterValue(&mqtt, unitId, deviceName,
                                                     registerName, value, MQTT_BASE_TOPIC "/modbus");
            
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
}

void loop() {
    // Run all feature loop handlers
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->loop();
    }
    
    // Publish Home Assistant autodiscovery once MQTT is connected
    if (mqtt.isConnected() && !haDiscoveryPublished) {
        DataCollectionMQTT::publishDiscovery(
            &mqtt,
            "sensors",
            sensorHAConfig,
            sizeof(sensorHAConfig) / sizeof(sensorHAConfig[0]),
            OTA_HOSTNAME,          // Device name in HA
            OTA_HOSTNAME,          // Device unique ID
            "Custom",              // Manufacturer
            "ESP32 Sensor Node",   // Model
            "1.0.0"                // Software version
        );
        haDiscoveryPublished = true;
        LOG_I("Home Assistant autodiscovery published");
    }
    
    // Publish Modbus Home Assistant autodiscovery once MQTT is connected
    if (mqtt.isConnected() && !modbusHADiscoveryPublished && modbusDevices) {
        ModbusIntegration::publishDiscovery(
            &mqtt,
            *modbusDevices,
            MQTT_BASE_TOPIC "/modbus",
            "Custom",              // Manufacturer
            "ESP32 Modbus Gateway", // Model
            "1.0.0"                // Software version
        );
        modbusHADiscoveryPublished = true;
        LOG_I("Modbus Home Assistant autodiscovery published");
    }
    
    // Periodic Modbus state publishing to MQTT
    if (mqtt.isConnected() && modbusDevices &&
        millis() - lastModbusStatePublish >= MODBUS_STATE_PUBLISH_INTERVAL) {
        lastModbusStatePublish = millis();
        ModbusIntegration::publishAllDeviceStates(&mqtt, *modbusDevices,
                                                   MQTT_BASE_TOPIC "/modbus");
    }
    
    // Periodic data collection
    if (millis() - lastDataCollection >= DATA_COLLECTION_INTERVAL) {
        lastDataCollection = millis();
        collectSensorData();
    }
    
    // Handle data collection persistence
    sensorData.loop();
    
    // Run Modbus device polling
    if (modbusDevices) {
        modbusDevices->loop();
    }
}
