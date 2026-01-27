# ESP32 Arduino PlatformIO Project Specification

## Project Overview

ESP32 firmware project using Arduino framework with PlatformIO build system. The project implements a modular feature-based architecture where each functionality is encapsulated in a class derived from a common base class.

## Project Structure

```
esp32-firmware/
├── platformio.ini
├── src/
│   ├── main.cpp
│   ├── Feature.h                 # Base class for all features
│   ├── WiFiManagerFeature.h
│   ├── WiFiManagerFeature.cpp
│   ├── LoggingFeature.h
│   ├── LoggingFeature.cpp
│   ├── TimeSyncFeature.h
│   ├── TimeSyncFeature.cpp
│   ├── WebServerFeature.h
│   ├── WebServerFeature.cpp
│   ├── OTAFeature.h
│   ├── OTAFeature.cpp
│   ├── StorageFeature.h
│   ├── StorageFeature.cpp
│   ├── InfluxDBFeature.h
│   ├── InfluxDBFeature.cpp
│   ├── MQTTFeature.h
│   ├── MQTTFeature.cpp
│   ├── DataCollection.h          # Template for typed data collections
│   ├── DataCollectionWeb.h       # Web endpoints for data collections
│   ├── DataCollectionMQTT.h      # MQTT/Home Assistant integration
│   ├── ModbusRTU.h               # Low-level Modbus RTU bus monitor
│   ├── ModbusRTU.cpp
│   ├── ModbusDevice.h            # High-level device definitions
│   ├── ModbusDevice.cpp
│   └── ModbusWeb.h               # Modbus web endpoints
├── data/
│   └── modbus/
│       ├── devices.json          # Unit ID to device type mapping
│       └── devices/
│           ├── sdm120.json                 # Eastron SDM120 single-phase meter
│           ├── sdm630.json                 # Eastron SDM630 three-phase meter
│           ├── sdm72.json                  # Eastron SDM72 three-phase meter
│           ├── solplanet_asw_hybrid.json   # Solplanet ASW 05-12k hybrid inverter
│           ├── solplanet_apollo_sol11h.json # Solplanet Apollo SOL 11H wallbox
│           └── solplanet_aihb_g2pro.json   # Solplanet Ai-HB G2 Pro battery
└── SPEC.md
```

## Feature Base Class

**Non-Blocking Design Principle:** Both `setup()` and `loop()` methods must be non-blocking. If an operation cannot complete immediately (e.g., waiting for network, sensor not ready), the method should return and retry on the next call. Use state machines or flags to track progress across multiple invocations.

```cpp
// Feature.h
#ifndef FEATURE_H
#define FEATURE_H

class Feature {
public:
    virtual ~Feature() = default;
    
    // Called once during setup phase
    // MUST be non-blocking - if not ready, return and retry on next loop()
    virtual void setup() = 0;
    
    // Called repeatedly in main loop (optional, default empty)
    // MUST be non-blocking - never use delay() or blocking waits
    virtual void loop() {}
    
    // Returns feature name for logging/debugging
    virtual const char* getName() const = 0;
    
    // Returns true when feature is fully initialized and operational
    virtual bool isReady() const { return true; }
};

#endif
```

## Features Specification

### 1. WiFiManagerFeature

**Purpose:** Handle WiFi connection with captive portal for configuration.

**Library:** `tzapu/WiFiManager`

**Constructor Parameters:**
- `const char* apName` - Access point name for configuration portal
- `const char* apPassword` - Access point password (optional, can be empty)
- `uint16_t configPortalTimeout` - Timeout in seconds for config portal

**Build Flags:**
```ini
-D WIFI_AP_NAME=\"ESP32-Config\"
-D WIFI_AP_PASSWORD=\"\"
-D WIFI_CONFIG_PORTAL_TIMEOUT=180
```

**Class Interface:**
```cpp
class WiFiManagerFeature : public Feature {
public:
    WiFiManagerFeature(const char* apName, const char* apPassword, uint16_t configPortalTimeout);
    void setup() override;
    void loop() override;  // Handle reconnection if needed
    const char* getName() const override { return "WiFiManager"; }
    
    bool isConnected() const;
    String getIPAddress() const;
};
```

### 2. LoggingFeature

**Purpose:** Centralized logging with serial output, parallel syslog output, and configurable boot/runtime log levels.

**Library:** None (custom implementation)

**Features:**
- Separate log levels for serial and syslog outputs
- Boot phase with higher verbosity, automatically transitioning to runtime level
- Parallel UDP syslog output (RFC 3164 BSD format)
- Timestamp support with NTP time or millis() fallback

**Constructor Parameters:**
- `uint32_t baudRate` - Serial baud rate
- `uint8_t serialBootLogLevel` - Log level for serial during boot phase
- `uint8_t serialRuntimeLogLevel` - Log level for serial after boot phase ends
- `uint32_t bootDurationMs` - Duration of boot phase in milliseconds
- `uint8_t syslogLogLevel` - Log level for syslog output
- `const char* syslogServer` - Syslog server hostname/IP (empty string = disabled)
- `uint16_t syslogPort` - Syslog server UDP port
- `const char* hostname` - Device hostname for syslog messages
- `bool enableTimestamp` - Include timestamp in log messages

**Build Flags:**
```ini
-D LOG_BAUD_RATE=115200
-D LOG_SERIAL_BOOT_LEVEL=4
-D LOG_SERIAL_RUNTIME_LEVEL=3
-D LOG_BOOT_DURATION_MS=30000
-D LOG_SYSLOG_LEVEL=3
-D LOG_SYSLOG_SERVER=\"\"
-D LOG_SYSLOG_PORT=514
-D LOG_ENABLE_TIMESTAMP=true
```

**Class Interface:**
```cpp
class LoggingFeature : public Feature {
public:
    LoggingFeature(uint32_t baudRate,
                   uint8_t serialBootLogLevel,
                   uint8_t serialRuntimeLogLevel,
                   uint32_t bootDurationMs,
                   uint8_t syslogLogLevel,
                   const char* syslogServer,
                   uint16_t syslogPort,
                   const char* hostname,
                   bool enableTimestamp);
    void setup() override;
    void loop() override;  // Handles boot-to-runtime transition
    const char* getName() const override { return "Logging"; }
    
    void error(const char* format, ...);
    void warn(const char* format, ...);
    void info(const char* format, ...);
    void debug(const char* format, ...);
    void verbose(const char* format, ...);
    
    static LoggingFeature* getInstance();  // Singleton access for global logging
    
    uint8_t getSerialLogLevel() const;
    void setSerialLogLevel(uint8_t level);
    uint8_t getSyslogLogLevel() const;
    void setSyslogLogLevel(uint8_t level);
    bool isSyslogEnabled() const;
    bool isBootPhase() const;
};

// Global convenience macros
#define LOG_E(...) LoggingFeature::getInstance()->error(__VA_ARGS__)
#define LOG_W(...) LoggingFeature::getInstance()->warn(__VA_ARGS__)
#define LOG_I(...) LoggingFeature::getInstance()->info(__VA_ARGS__)
#define LOG_D(...) LoggingFeature::getInstance()->debug(__VA_ARGS__)
#define LOG_V(...) LoggingFeature::getInstance()->verbose(__VA_ARGS__)
```

### 3. TimeSyncFeature

**Purpose:** Synchronize system time via NTP.

**Library:** Built-in ESP32 SNTP

**Constructor Parameters:**
- `const char* ntpServer1` - Primary NTP server
- `const char* ntpServer2` - Secondary NTP server
- `const char* timezone` - POSIX timezone string
- `uint32_t syncIntervalMs` - Re-sync interval in milliseconds

**Build Flags:**
```ini
-D NTP_SERVER1=\"pool.ntp.org\"
-D NTP_SERVER2=\"time.nist.gov\"
-D TIMEZONE=\"CET-1CEST,M3.5.0,M10.5.0/3\"
-D NTP_SYNC_INTERVAL=3600000
```

**Class Interface:**
```cpp
class TimeSyncFeature : public Feature {
public:
    TimeSyncFeature(const char* ntpServer1, const char* ntpServer2, 
                    const char* timezone, uint32_t syncIntervalMs);
    void setup() override;
    void loop() override;  // Periodic re-sync check
    const char* getName() const override { return "TimeSync"; }
    
    bool isSynced() const;
    String getFormattedTime() const;
    time_t getEpochTime() const;
};
```

### 4. WebServerFeature

**Purpose:** Async web server for REST API and web interface.

**Library:** `me-no-dev/ESPAsyncWebServer` + `me-no-dev/AsyncTCP`

**Constructor Parameters:**
- `uint16_t port` - HTTP server port
- `const char* username` - Basic auth username (empty = no auth)
- `const char* password` - Basic auth password

**Build Flags:**
```ini
-D WEBSERVER_PORT=80
-D WEBSERVER_USERNAME=\"admin\"
-D WEBSERVER_PASSWORD=\"admin\"
```

**Class Interface:**
```cpp
class WebServerFeature : public Feature {
public:
    WebServerFeature(uint16_t port, const char* username, const char* password);
    void setup() override;
    const char* getName() const override { return "WebServer"; }
    
    AsyncWebServer* getServer();  // Access server for adding routes
    void addHandler(AsyncWebHandler* handler);
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
};
```

### 5. OTAFeature

**Purpose:** Over-the-air firmware updates via ArduinoOTA.

**Library:** Built-in `ArduinoOTA`

**Constructor Parameters:**
- `const char* hostname` - mDNS hostname for OTA discovery
- `const char* password` - OTA password (empty = no password)
- `uint16_t port` - OTA port

**Build Flags:**
```ini
-D OTA_HOSTNAME=\"esp32-device\"
-D OTA_PASSWORD=\"otapassword\"
-D OTA_PORT=3232
```

**Class Interface:**
```cpp
class OTAFeature : public Feature {
public:
    OTAFeature(const char* hostname, const char* password, uint16_t port);
    void setup() override;
    void loop() override;  // Handle OTA requests
    const char* getName() const override { return "OTA"; }
};
```

### 6. StorageFeature

**Purpose:** LittleFS filesystem wrapper for persistent storage.

**Library:** Built-in `LittleFS`

**Constructor Parameters:**
- `bool formatOnFail` - Format filesystem if mount fails

**Class Interface:**
```cpp
class StorageFeature : public Feature {
public:
    StorageFeature(bool formatOnFail = true);
    void setup() override;
    const char* getName() const override { return "Storage"; }
    bool isReady() const override;
    
    bool writeFile(const char* path, const String& content);
    String readFile(const char* path);
    bool appendFile(const char* path, const String& content);
    bool exists(const char* path);
    bool remove(const char* path);
    bool mkdir(const char* path);
    void listDir(const char* path, std::vector<String>& files);
};
```

### 7. InfluxDBFeature

**Purpose:** Write data to InfluxDB using line protocol over HTTP.

**Supports:** Both InfluxDB 1.x (user/password) and 2.x (org/bucket/token)

**Library:** Built-in `HTTPClient`

**InfluxDB 1.x Build Flags:**
```ini
-D INFLUXDB_URL=\"http://192.168.1.100:8086\"
-D INFLUXDB_VERSION=1
-D INFLUXDB_DATABASE=\"sensors\"
-D INFLUXDB_USERNAME=\"myuser\"
-D INFLUXDB_PASSWORD=\"mypassword\"
-D INFLUXDB_RP=\"\"
-D INFLUXDB_BATCH_INTERVAL=10000
-D INFLUXDB_BATCH_SIZE=50
```

**InfluxDB 2.x Build Flags:**
```ini
-D INFLUXDB_URL=\"http://192.168.1.100:8086\"
-D INFLUXDB_VERSION=2
-D INFLUXDB_ORG=\"my-org\"
-D INFLUXDB_BUCKET=\"my-bucket\"
-D INFLUXDB_TOKEN=\"my-api-token\"
-D INFLUXDB_BATCH_INTERVAL=10000
-D INFLUXDB_BATCH_SIZE=50
```

**Class Interface:**
```cpp
class InfluxDBFeature : public Feature {
public:
    // InfluxDB 2.x constructor
    InfluxDBFeature(const char* serverUrl, const char* org, const char* bucket,
                    const char* token, uint32_t batchIntervalMs = 10000,
                    size_t batchSize = 100);
    
    // InfluxDB 1.x factory method
    static InfluxDBFeature createV1(const char* serverUrl, const char* database,
                                     const char* username = "", const char* password = "",
                                     const char* retentionPolicy = "",
                                     uint32_t batchIntervalMs = 10000,
                                     size_t batchSize = 100);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "InfluxDB"; }
    
    void queue(const String& lineProtocol);  // Queue line protocol data
    bool upload();                            // Force immediate upload
    bool isConnected() const;
    size_t pendingCount() const;
};
```

**Usage in main.cpp:**
```cpp
// InfluxDB 1.x (configured via INFLUXDB_VERSION=1)
InfluxDBFeature influxDB = InfluxDBFeature::createV1(
    INFLUXDB_URL, INFLUXDB_DATABASE,
    INFLUXDB_USERNAME, INFLUXDB_PASSWORD, INFLUXDB_RP,
    INFLUXDB_BATCH_INTERVAL, INFLUXDB_BATCH_SIZE
);

// InfluxDB 2.x (configured via INFLUXDB_VERSION=2)
InfluxDBFeature influxDB(
    INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN,
    INFLUXDB_BATCH_INTERVAL, INFLUXDB_BATCH_SIZE
);
```

### 8. DataCollectionWeb

**Purpose:** Automatically register web endpoints for DataCollection instances.

**Library:** Requires `ESPAsyncWebServer`

**Features:**
- JSON API endpoint for all data (`/api/<basePath>`)
- JSON API endpoint for latest entry (`/api/<basePath>/latest`)
- Interactive HTML table view (`/view/<basePath>`)
- Dark theme with responsive design
- Auto-refresh with configurable interval
- Connection status indicator
- Timestamp formatting

**Static Methods:**
```cpp
class DataCollectionWeb {
public:
    // Register endpoints with custom callbacks
    static void registerEndpoints(
        AsyncWebServer* server,
        const char* basePath,
        std::function<String()> getJsonCallback,
        std::function<String()> getLatestJsonCallback,
        std::function<String()> getSchemaCallback,
        uint32_t refreshIntervalMs = 5000
    );
    
    // Convenience method for DataCollection instances
    template<typename T, size_t N>
    static void registerCollection(
        AsyncWebServer* server,
        DataCollection<T, N>& collection,
        const char* basePath,
        uint32_t refreshIntervalMs = 5000
    );
};
```

**Usage Example:**
```cpp
// In setup(), after webServer.setup():
DataCollectionWeb::registerCollection(
    webServer.getServer(),
    sensorData,           // DataCollection<SensorData, 100>
    "sensors",            // Creates /api/sensors, /view/sensors
    5000                  // Auto-refresh every 5 seconds
);
```

### 9. MQTTFeature

**Purpose:** MQTT client with auto-reconnect for publishing sensor data.

**Library:** `knolleary/PubSubClient`

**Constructor Parameters:**
- `const char* server` - MQTT broker hostname/IP
- `uint16_t port` - MQTT broker port
- `const char* username` - MQTT username (empty = no auth)
- `const char* password` - MQTT password
- `const char* clientId` - MQTT client identifier
- `const char* baseTopic` - Base topic for all messages
- `uint32_t reconnectIntervalMs` - Reconnect attempt interval

**Build Flags:**
```ini
-D MQTT_SERVER=\"\"
-D MQTT_PORT=1883
-D MQTT_USERNAME=\"\"
-D MQTT_PASSWORD=\"\"
-D MQTT_BASE_TOPIC=\"esp32\"
-D MQTT_RECONNECT_INTERVAL=5000
```

**Class Interface:**
```cpp
class MQTTFeature : public Feature {
public:
    MQTTFeature(const char* server, uint16_t port,
                const char* username, const char* password,
                const char* clientId, const char* baseTopic,
                uint32_t reconnectIntervalMs = 5000);
    void setup() override;
    void loop() override;  // Handle reconnection and message processing
    const char* getName() const override { return "MQTT"; }
    bool isReady() const override { return _connected; }
    
    bool publish(const char* topic, const char* payload, bool retain = false);
    bool publishToBase(const char* subtopic, const char* payload, bool retain = false);
    bool subscribe(const char* topic);
    void onMessage(MessageCallback callback);
    
    bool isConnected() const;
    const char* getBaseTopic() const;
    static MQTTFeature* getInstance();
};
```

### 10. DataCollectionMQTT

**Purpose:** Home Assistant autodiscovery integration for data collections.

**Features:**
- Publishes HA-compatible discovery config to `homeassistant/sensor/...`
- State updates to `<base_topic>/<collection>/state`
- Availability tracking via `<base_topic>/status`
- Device grouping (all sensors appear as one device in HA)

**Static Methods:**
```cpp
class DataCollectionMQTT {
public:
    // Publish Home Assistant autodiscovery config
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
    );
    
    // Publish latest data entry
    template<typename T, size_t N>
    static void publishLatest(MQTTFeature* mqtt, DataCollection<T, N>& collection, const char* name);
    
    // Remove discovery config
    static void removeDiscovery(MQTTFeature* mqtt, const char* name, 
                                const HASensorConfig* configs, size_t count, const char* deviceId);
};

// Sensor configuration for Home Assistant
struct HASensorConfig {
    const char* fieldName;      // Field name in DataCollection
    const char* displayName;    // Human-readable name in HA
    const char* deviceClass;    // HA device class (HADeviceClass::*)
    const char* unit;           // Unit of measurement
    const char* icon;           // MDI icon (optional)
};
```

**Available Device Classes (HADeviceClass namespace):**
- `TEMPERATURE`, `HUMIDITY`, `PRESSURE`
- `BATTERY`, `VOLTAGE`, `CURRENT`, `POWER`, `ENERGY`
- `SIGNAL_STRENGTH`, `ILLUMINANCE`
- `CO2`, `PM25`, `PM10`
- `TIMESTAMP`, `DURATION`

**Usage Example:**
```cpp
const HASensorConfig sensorHAConfig[] = {
    { "temperature", "Temperature", HADeviceClass::TEMPERATURE, "°C", nullptr },
    { "humidity", "Humidity", HADeviceClass::HUMIDITY, "%", nullptr },
    { "rssi", "WiFi Signal", HADeviceClass::SIGNAL_STRENGTH, "dBm", nullptr },
};

// Publish discovery once when MQTT connects
DataCollectionMQTT::publishDiscovery(&mqtt, "sensors", sensorHAConfig, 3,
    "Living Room", "esp32-living", "Custom", "ESP32 Node", "1.0.0");

// Publish data on each collection
DataCollectionMQTT::publishLatest(&mqtt, sensorData, "sensors");
```

### 10. ModbusRTUFeature

**Purpose:** Monitor Modbus RTU bus traffic, maintain register maps per unit/function code, and send Modbus requests with proper bus silence detection.

**Library:** Built-in (no external library)

**Constructor Parameters:**
- `HardwareSerial& serial` - Hardware serial port (Serial1, Serial2)
- `uint32_t baudRate` - Baud rate (typically 9600, 19200)
- `uint32_t config` - Serial config (SERIAL_8N1, SERIAL_8E1)
- `int8_t rxPin` - RX pin (-1 for default)
- `int8_t txPin` - TX pin (-1 for default)
- `int8_t dePin` - RS485 DE pin (-1 if not used)
- `size_t maxQueueSize` - Maximum pending requests
- `uint32_t responseTimeoutMs` - Response timeout

**Build Flags:**
```ini
-D MODBUS_SERIAL_RX=16
-D MODBUS_SERIAL_TX=17
-D MODBUS_BAUD_RATE=9600
-D MODBUS_SERIAL_CONFIG=SERIAL_8N1
-D MODBUS_DE_PIN=-1
-D MODBUS_RESPONSE_TIMEOUT=1000
-D MODBUS_QUEUE_SIZE=10
-D MODBUS_DEVICE_TYPES_PATH=\"/modbus/devices\"
-D MODBUS_DEVICE_MAP_PATH=\"/modbus/devices.json\"
```

**Class Interface:**
```cpp
class ModbusRTUFeature : public Feature {
public:
    ModbusRTUFeature(HardwareSerial& serial, uint32_t baudRate, uint32_t config,
                     int8_t rxPin, int8_t txPin, int8_t dePin,
                     size_t maxQueueSize, uint32_t responseTimeoutMs);
    void setup() override;
    void loop() override;
    const char* getName() const override { return "ModbusRTU"; }
    
    // Bus state
    bool isBusSilent() const;
    unsigned long getTimeSinceLastActivity() const;
    
    // Register callback for all frames
    void onFrame(std::function<void(const ModbusFrame&, bool isRequest)> callback);
    
    // Get register map for unit/FC combination
    ModbusRegisterMap* getRegisterMap(uint8_t unitId, uint8_t functionCode);
    const std::map<uint16_t, ModbusRegisterMap>& getAllRegisterMaps() const;
    
    // Queue requests (waits for bus silence automatically)
    bool queueReadRegisters(uint8_t unitId, uint8_t fc, uint16_t startReg, uint16_t qty,
                            std::function<void(bool, const ModbusFrame&)> callback);
    bool queueWriteSingleRegister(uint8_t unitId, uint16_t addr, uint16_t value,
                                   std::function<void(bool, const ModbusFrame&)> callback);
    bool queueWriteMultipleRegisters(uint8_t unitId, uint16_t startAddr,
                                      const std::vector<uint16_t>& values,
                                      std::function<void(bool, const ModbusFrame&)> callback);
    
    size_t getPendingRequestCount() const;
    void clearQueue();
    
    struct Stats { uint32_t framesReceived, framesSent, crcErrors, timeouts, queueOverflows; };
    const Stats& getStats() const;
};
```

### 11. ModbusDeviceManager

**Purpose:** High-level device abstraction reading register definitions from filesystem.

**Device Type Definition Format** (`/modbus/devices/sdm120.json`):
```json
{
    "name": "SDM120",
    "registers": [
        {
            "name": "Voltage",
            "address": 0,
            "length": 2,
            "functionCode": 4,
            "dataType": "float32_be",
            "factor": 1.0,
            "offset": 0,
            "unit": "V",
            "pollInterval": 5000
        }
    ]
}
```

**Device Mapping Format** (`/modbus/devices.json`):
```json
{
    "devices": [
        {"unitId": 1, "type": "SDM120", "name": "Main Meter"},
        {"unitId": 2, "type": "SDM120", "name": "Solar Meter"}
    ]
}
```

**Supported Data Types:**
- `uint16`, `int16`
- `uint32_be`, `uint32_le`, `int32_be`, `int32_le`
- `float32_be`, `float32_le`
- `bool`, `string`

**Class Interface:**
```cpp
class ModbusDeviceManager {
public:
    ModbusDeviceManager(ModbusRTUFeature& modbus, StorageFeature& storage);
    
    bool loadDeviceType(const char* path);
    bool loadDeviceMappings(const char* path);
    bool loadAllDeviceTypes(const char* directory);
    
    const ModbusDeviceType* getDeviceType(const char* name) const;
    ModbusDeviceInstance* getDevice(uint8_t unitId);
    const std::map<uint8_t, ModbusDeviceInstance>& getDevices() const;
    
    bool readRegister(uint8_t unitId, const char* registerName,
                      std::function<void(bool, float)> callback);
    bool readAllRegisters(uint8_t unitId, std::function<void(bool)> callback);
    bool writeRegister(uint8_t unitId, const char* registerName, float value,
                       std::function<void(bool)> callback);
    
    bool getValue(uint8_t unitId, const char* registerName, float& value) const;
    String getDeviceValuesJson(uint8_t unitId) const;
    
    void loop();  // Handle automatic polling
    std::vector<String> getDeviceTypeNames() const;
};
```

**Web Endpoints (via ModbusWeb):**
- `GET /api/modbus/devices` - List all mapped devices
- `GET /api/modbus/device?unit=1` - Get device values
- `GET /api/modbus/read?unit=1&register=Voltage` - Read register
- `POST /api/modbus/write` - Write register
- `GET /api/modbus/status` - Bus status and statistics
- `GET /api/modbus/maps` - Raw register maps from monitoring
- `GET /api/modbus/types` - List loaded device types
- `GET /view/modbus` - HTML dashboard

## PlatformIO Configuration

```ini
; platformio.ini

[platformio]
default_envs = serial

; ============================================
; Shared configuration for all environments
; ============================================
[env]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

; Library dependencies
lib_deps = 
    tzapu/WiFiManager@^2.0.17
    me-no-dev/ESPAsyncWebServer@^1.2.4
    me-no-dev/AsyncTCP@^1.1.1

; Common build flags
build_flags = 
    ; Logging configuration
    -D LOG_BAUD_RATE=115200
    -D LOG_LEVEL=4
    -D LOG_ENABLE_TIMESTAMP=true
    
    ; WiFi configuration
    -D WIFI_AP_NAME=\"ESP32-Config\"
    -D WIFI_AP_PASSWORD=\"\"
    -D WIFI_CONFIG_PORTAL_TIMEOUT=180
    
    ; NTP / Time configuration
    -D NTP_SERVER1=\"pool.ntp.org\"
    -D NTP_SERVER2=\"time.nist.gov\"
    -D TIMEZONE=\"CET-1CEST,M3.5.0,M10.5.0/3\"
    -D NTP_SYNC_INTERVAL=3600000
    
    ; Web server configuration
    -D WEBSERVER_PORT=80
    -D WEBSERVER_USERNAME=\"admin\"
    -D WEBSERVER_PASSWORD=\"admin\"
    
    ; OTA configuration
    -D OTA_HOSTNAME=\"esp32-device\"
    -D OTA_PASSWORD=\"otapassword\"
    -D OTA_PORT=3232

; ============================================
; Serial upload environment
; ============================================
[env:serial]
upload_protocol = esptool
upload_port = /dev/ttyUSB0
; Alternatively auto-detect:
; upload_port = /dev/ttyUSB*

; ============================================
; OTA upload environment
; ============================================
[env:ota]
upload_protocol = espota
upload_port = esp32-device.local
; Or use IP address:
; upload_port = 192.168.1.100
upload_flags = 
    --port=${common.ota_port}
    --auth=${common.ota_password}

[common]
ota_port = 3232
ota_password = otapassword
```

## Main Application Entry Point

```cpp
// main.cpp
#include <Arduino.h>
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "OTAFeature.h"

// Feature instances
LoggingFeature logging(LOG_BAUD_RATE, LOG_LEVEL, LOG_ENABLE_TIMESTAMP);
WiFiManagerFeature wifiManager(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_CONFIG_PORTAL_TIMEOUT);
TimeSyncFeature timeSync(NTP_SERVER1, NTP_SERVER2, TIMEZONE, NTP_SYNC_INTERVAL);
WebServerFeature webServer(WEBSERVER_PORT, WEBSERVER_USERNAME, WEBSERVER_PASSWORD);
OTAFeature ota(OTA_HOSTNAME, OTA_PASSWORD, OTA_PORT);

// Array of all features for easy iteration
Feature* features[] = {
    &logging,      // Must be first for early logging
    &wifiManager,  // Must be before network-dependent features
    &timeSync,
    &webServer,
    &ota
};
const size_t featureCount = sizeof(features) / sizeof(features[0]);

void setup() {
    // Initialize all features
    for (size_t i = 0; i < featureCount; i++) {
        LOG_I("Setting up feature: %s", features[i]->getName());
        features[i]->setup();
    }
    LOG_I("All features initialized");
}

void loop() {
    // Run all feature loop handlers
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->loop();
    }
}
```

## Build Flag Defaults Summary

| Feature | Flag | Default Value |
|---------|------|---------------|
| Logging | `LOG_BAUD_RATE` | `115200` |
| Logging | `LOG_LEVEL` | `4` (DEBUG) |
| Logging | `LOG_ENABLE_TIMESTAMP` | `true` |
| WiFi | `WIFI_AP_NAME` | `"ESP32-Config"` |
| WiFi | `WIFI_AP_PASSWORD` | `""` (open) |
| WiFi | `WIFI_CONFIG_PORTAL_TIMEOUT` | `180` seconds |
| NTP | `NTP_SERVER1` | `"pool.ntp.org"` |
| NTP | `NTP_SERVER2` | `"time.nist.gov"` |
| NTP | `TIMEZONE` | `"CET-1CEST,M3.5.0,M10.5.0/3"` |
| NTP | `NTP_SYNC_INTERVAL` | `3600000` ms (1 hour) |
| WebServer | `WEBSERVER_PORT` | `80` |
| WebServer | `WEBSERVER_USERNAME` | `"admin"` |
| WebServer | `WEBSERVER_PASSWORD` | `"admin"` |
| OTA | `OTA_HOSTNAME` | `"esp32-device"` |
| OTA | `OTA_PASSWORD` | `"otapassword"` |
| OTA | `OTA_PORT` | `3232` |
| InfluxDB | `INFLUXDB_URL` | `""` (disabled) |
| InfluxDB | `INFLUXDB_VERSION` | `1` (1.x or 2) |
| InfluxDB | `INFLUXDB_DATABASE` | `""` (V1 only) |
| InfluxDB | `INFLUXDB_USERNAME` | `""` (V1 only) |
| InfluxDB | `INFLUXDB_PASSWORD` | `""` (V1 only) |
| InfluxDB | `INFLUXDB_RP` | `""` (V1 retention policy) |
| InfluxDB | `INFLUXDB_ORG` | `""` (V2 only) |
| InfluxDB | `INFLUXDB_BUCKET` | `""` (V2 only) |
| InfluxDB | `INFLUXDB_TOKEN` | `""` (V2 only) |
| InfluxDB | `INFLUXDB_BATCH_INTERVAL` | `10000` ms |
| InfluxDB | `INFLUXDB_BATCH_SIZE` | `50` |
| MQTT | `MQTT_SERVER` | `""` (disabled) |
| MQTT | `MQTT_PORT` | `1883` |
| MQTT | `MQTT_USERNAME` | `""` |
| MQTT | `MQTT_PASSWORD` | `""` |
| MQTT | `MQTT_BASE_TOPIC` | `"esp32"` |
| MQTT | `MQTT_RECONNECT_INTERVAL` | `5000` ms |
| Modbus | `MODBUS_SERIAL_RX` | `16` |
| Modbus | `MODBUS_SERIAL_TX` | `17` |
| Modbus | `MODBUS_BAUD_RATE` | `9600` |
| Modbus | `MODBUS_SERIAL_CONFIG` | `SERIAL_8N1` |
| Modbus | `MODBUS_DE_PIN` | `-1` (none) |
| Modbus | `MODBUS_RESPONSE_TIMEOUT` | `1000` ms |
| Modbus | `MODBUS_QUEUE_SIZE` | `10` |
| Modbus | `MODBUS_DEVICE_TYPES_PATH` | `"/modbus/devices"` |
| Modbus | `MODBUS_DEVICE_MAP_PATH` | `"/modbus/devices.json"` |

## Dependencies

| Library | Version | Purpose |
|---------|---------|----------|
| `tzapu/WiFiManager` | ^2.0.17 | WiFi configuration portal |
| `me-no-dev/ESPAsyncWebServer` | ^1.2.4 | Async HTTP server |
| `me-no-dev/AsyncTCP` | ^1.1.1 | Async TCP for ESP32 |
| `bblanchon/ArduinoJson` | ^7.0.0 | JSON serialization |
| `knolleary/PubSubClient` | ^2.8 | MQTT client |

## Notes

1. **Feature Initialization Order:** LoggingFeature must be initialized first, followed by WiFiManagerFeature before any network-dependent features.

2. **String Literals in Build Flags:** Use escaped quotes `\"value\"` for string values in build flags.

3. **Boolean Build Flags:** Use `true`/`false` which will be interpreted as `1`/`0` by the preprocessor.

4. **OTA Environment:** Ensure the device is connected to the network and reachable before using OTA upload.

5. **Serial Port:** Adjust `upload_port` in `[env:serial]` to match your system (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux).

6. **mDNS:** The OTA hostname uses mDNS, ensure your network supports it or use IP address instead.
