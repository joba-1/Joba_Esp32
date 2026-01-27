#include <Arduino.h>
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "OTAFeature.h"
#include "StorageFeature.h"
#include "InfluxDBFeature.h"
#include "DataCollection.h"

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
InfluxDBFeature influxDB(
    INFLUXDB_URL,
    INFLUXDB_ORG,
    INFLUXDB_BUCKET,
    INFLUXDB_TOKEN,
    INFLUXDB_BATCH_INTERVAL,
    INFLUXDB_BATCH_SIZE
);

// Array of all features for easy iteration
Feature* features[] = {
    &logging,      // Must be first for early logging
    &wifiManager,  // Must be before network-dependent features
    &timeSync,
    &storage,      // Filesystem before features that need it
    &webServer,
    &ota,
    &influxDB
};
const size_t featureCount = sizeof(features) / sizeof(features[0]);

// ============================================
// Data Collection Timer
// ============================================
unsigned long lastDataCollection = 0;
const unsigned long DATA_COLLECTION_INTERVAL = 60000;  // Collect every 60 seconds

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
    
    // Add API endpoint for sensor data
    webServer.getServer()->on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", sensorData.toJson());
    });
    
    // Add API endpoint for latest reading
    webServer.getServer()->on("/api/sensors/latest", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (sensorData.isEmpty()) {
            request->send(404, "application/json", "{\"error\":\"No data\"}");
        } else {
            request->send(200, "application/json", sensorData.toJson(sensorData.count() - 1));
        }
    });
    
    LOG_I("All features initialized");
    LOG_I("Free heap: %d bytes", ESP.getFreeHeap());
}

void loop() {
    // Run all feature loop handlers
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->loop();
    }
    
    // Periodic data collection
    if (millis() - lastDataCollection >= DATA_COLLECTION_INTERVAL) {
        lastDataCollection = millis();
        collectSensorData();
    }
    
    // Handle data collection persistence
    sensorData.loop();
}
