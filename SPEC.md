# ESP32 Arduino PlatformIO Project Specification

## Project Overview

ESP32 firmware project using Arduino framework with PlatformIO build system. The project implements:

- **Modular feature-based architecture** with Feature base class pattern
- **Automatic device identification** based on firmware name and MAC address
- **Configuration management** split between build-essential (platformio.ini) and user-specific (config.ini)
- **Zero-configuration deployment** with auto-generated device identities, passwords, and MQTT topics
- **Time-series data logging** to InfluxDB with automatic device tagging
- **Home Assistant integration** via MQTT autodiscovery
- **Modbus RTU support** with JSON-based device definitions

## Project Structure

```
esp32-firmware/
├── platformio.ini              # Build configuration (references config.ini)
├── config.ini                  # User-specific settings (gitignored)
├── config.ini.template         # Template for user configuration
├── pre_build.py                # Auto-creates config.ini from template
├── src/
│   ├── main.cpp                # Device identity setup and feature orchestration
│   ├── DeviceInfo.h            # Device identification helper (NEW)
│   ├── Feature.h               # Base class for all features
│   ├── WiFiManagerFeature.h/cpp
│   ├── LoggingFeature.h/cpp
│   ├── TimeSyncFeature.h/cpp
│   ├── WebServerFeature.h/cpp
│   ├── OTAFeature.h/cpp
│   ├── StorageFeature.h/cpp
│   ├── InfluxDBFeature.h/cpp
│   ├── MQTTFeature.h/cpp
│   ├── LEDFeature.h/cpp        # Visual activity indicator (NEW)
│   ├── DataCollection.h        # Template for typed data collections
│   ├── DataCollectionWeb.h     # Web endpoints for data collections
│   ├── DataCollectionMQTT.h    # MQTT/Home Assistant integration
│   ├── ModbusRTU.h/cpp         # Low-level Modbus RTU bus monitor
│   ├── ModbusDevice.h/cpp      # High-level device definitions
│   └── ModbusWeb.h             # Modbus web endpoints
├── data/
│   └── modbus/
│       ├── devices.json        # Unit ID to device type mapping
│       └── devices/            # Device definition JSON files
└── SPEC.md
```

## Configuration Management

### Configuration Split

The project separates build-essential settings from user-specific configuration:

**platformio.ini** - Build settings and references to config.ini:
- Platform, board, framework
- Library dependencies
- Build flags that reference `${user_config.setting_name}`
- Pre-build script reference

**config.ini** - User-specific settings (gitignored):
- Firmware identity (name, version, device instance)
- Passwords and credentials
- Network settings (WiFi, NTP, syslog)
- Service endpoints (InfluxDB, MQTT)
- Hardware configuration (Modbus pins, LED)

**config.ini.template** - Template for user configuration:
- Committed to git with sensible defaults
- Documentation for each setting
- Copied to config.ini by pre_build.py if missing

### Pre-Build Script

**File:** `pre_build.py`

**Purpose:** Auto-create config.ini from template on first build

```python
Import("env")
import shutil
from pathlib import Path

def copy_config_template(source, target, env):
    config_file = Path("config.ini")
    template_file = Path("config.ini.template")
    
    if not config_file.exists() and template_file.exists():
        shutil.copy(template_file, config_file)
        print("=" * 60)
        print("Created config.ini from template")
        print("Please edit config.ini to customize your settings")
        print("=" * 60)

env.AddPreAction("$BUILD_DIR/src/main.cpp.o", copy_config_template)
```

**Integration in platformio.ini:**
```ini
extra_scripts = pre:pre_build.py
extra_configs = config.ini
```

### config.ini Structure

```ini
[user_config]
# Firmware Identity
firmware_name = ESP32-Firmware
firmware_version = 1.0.0
device_instance = 0
default_password = 

# Logging
log_serial_boot_level = 4
log_serial_runtime_level = 3
log_boot_duration_ms = 300000
log_syslog_level = 3
syslog_server = 

# Network
wifi_config_portal_timeout = 180
ntp_server1 = de.pool.ntp.org
ntp_server2 = europe.pool.ntp.org
timezone = CET-1CEST,M3.5.0,M10.5.0/3

# InfluxDB
influxdb_url = http://192.168.1.100:8086
influxdb_version = 1
influxdb_database = 
influxdb_username = 
influxdb_password = 

# MQTT (base topic auto-generated per device)
mqtt_server = 
mqtt_username = 
mqtt_password = 

# Modbus RTU
modbus_serial_rx = 16
modbus_serial_tx = 17
modbus_baud_rate = 9600
modbus_de_pin = -1

# LED Indicator
led_pin = 2
led_active_low = true
```

### Build Flags in platformio.ini

Build flags reference config.ini values:

```ini
build_flags = 
    -D FIRMWARE_NAME=\"${user_config.firmware_name}\"
    -D FIRMWARE_VERSION=\"${user_config.firmware_version}\"
    -D DEVICE_INSTANCE=${user_config.device_instance}
    -D DEFAULT_PASSWORD=\"${user_config.default_password}\"
    -D LOG_BAUD_RATE=115200
    -D LOG_SERIAL_BOOT_LEVEL=${user_config.log_serial_boot_level}
    -D LOG_SERIAL_RUNTIME_LEVEL=${user_config.log_serial_runtime_level}
    -D LOG_BOOT_DURATION_MS=${user_config.log_boot_duration_ms}
    -D LOG_SYSLOG_LEVEL=${user_config.log_syslog_level}
    ; ... etc
```

## Device Identity System

### DeviceInfo Helper (NEW)

**File:** `src/DeviceInfo.h`

**Purpose:** Centralized device identity generation based on firmware name and MAC address

**Implementation:**
```cpp
#ifndef DEVICEINFO_H
#define DEVICEINFO_H

#include <Arduino.h>
#include <WiFi.h>

class DeviceInfo {
public:
    // Get unique device ID: "FirmwareName-A1B2C3" or "FirmwareName-NN"
    static String getDeviceId() {
        static String deviceId;
        if (deviceId.isEmpty()) {
            if (DEVICE_INSTANCE == 0) {
                deviceId = String(FIRMWARE_NAME) + "-" + getMacSuffix();
            } else {
                deviceId = String(FIRMWARE_NAME) + "-" + String(DEVICE_INSTANCE);
            }
        }
        return deviceId;
    }
    
    // Get hostname: "firmware-name-a1b2c3" (lowercase, hyphenated)
    static String getHostname() {
        static String hostname;
        if (hostname.isEmpty()) {
            hostname = getDeviceId();
            hostname.toLowerCase();
            hostname.replace("_", "-");
            hostname.replace(" ", "-");
        }
        return hostname;
    }
    
    // Get default password: "FirmwareName-A1B2C3" or custom
    static String getDefaultPassword() {
        static String password;
        if (password.isEmpty()) {
            String configPassword = DEFAULT_PASSWORD;
            if (configPassword.length() > 0) {
                password = configPassword;
            } else {
                password = getDeviceId();
            }
        }
        return password;
    }
    
    // Get last 3 bytes of MAC as hex: "A1B2C3"
    static String getMacSuffix() {
        static String suffix;
        if (suffix.isEmpty()) {
            uint8_t mac[6];
            WiFi.macAddress(mac);
            char buffer[7];
            sprintf(buffer, "%02X%02X%02X", mac[3], mac[4], mac[5]);
            suffix = String(buffer);
        }
        return suffix;
    }
    
    static const char* getFirmwareName() { return FIRMWARE_NAME; }
    static const char* getFirmwareVersion() { return FIRMWARE_VERSION; }
};

#endif
```

**Usage in main.cpp:**

```cpp
#include "DeviceInfo.h"

// Global device identity strings
String deviceId;
String hostname;
String apName;
String mqttClientId;
String mqttBaseTopic;
String defaultPassword;

void setup() {
    // Enable WiFi station mode to get MAC address
    WiFi.mode(WIFI_STA);
    delay(100);
    
    // Generate device identity
    deviceId = DeviceInfo::getDeviceId();
    hostname = DeviceInfo::getHostname();
    apName = deviceId + "-Setup";
    mqttClientId = hostname;
    mqttBaseTopic = String(FIRMWARE_NAME) + "/" + hostname;
    defaultPassword = DeviceInfo::getDefaultPassword();
    
    // Log device identity
    LOG_I("Device ID: %s", deviceId.c_str());
    LOG_I("Hostname: %s", hostname.c_str());
    LOG_I("Password: %s", defaultPassword.c_str());
    LOG_I("MQTT Base Topic: %s", mqttBaseTopic.c_str());
    
    // Configure features with dynamic values
    wifi.setAPName(apName.c_str());
    wifi.setAPPassword(defaultPassword.c_str());
    ota.setHostname(hostname.c_str());
    ota.setPassword(defaultPassword.c_str());
    logger.setHostname(hostname.c_str());
    mqtt.setClientId(mqttClientId.c_str());
    mqtt.setBaseTopic(mqttBaseTopic.c_str());
    webServer.setPassword(defaultPassword.c_str());
    
    // Set device ID on data collections for InfluxDB tagging
    sensorData.setDeviceId(deviceId.c_str());
    modbusData.setDeviceId(deviceId.c_str());
    
    // ... setup features
}
```

### Dynamic Device Properties

All device-specific strings are derived from DeviceInfo:

| Property | Format | Example |
|----------|--------|---------|
| Device ID | `{FIRMWARE_NAME}-{MAC_SUFFIX}` | `ESP32-Firmware-A1B2C3` |
| Hostname | `{firmware-name}-{mac-suffix}` (lowercase) | `esp32-firmware-a1b2c3` |
| AP Name | `{DeviceID}-Setup` | `ESP32-Firmware-A1B2C3-Setup` |
| mDNS | `{hostname}.local` | `esp32-firmware-a1b2c3.local` |
| Default Password | `{DeviceID}` or custom | `ESP32-Firmware-A1B2C3` |
| MQTT Client ID | `{hostname}` | `esp32-firmware-a1b2c3` |
| MQTT Base Topic | `{FIRMWARE_NAME}/{hostname}` | `ESP32-Firmware/esp32-firmware-a1b2c3` |

**Benefits:**
- Unique device identity across multiple instances
- No manual configuration required
- No conflicts on shared MQTT brokers
- Easy identification in logs, web interfaces, Home Assistant

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
- `const char* apName` - Access point name for configuration portal (set dynamically)
- `const char* apPassword` - Access point password (set dynamically from DeviceInfo)
- `uint16_t configPortalTimeout` - Timeout in seconds for config portal

**Build Flags:**
```ini
wifi_config_portal_timeout = 180
```

**Dynamic Configuration:**
```cpp
// In main.cpp setup()
String apName = DeviceInfo::getDeviceId() + "-Setup";  // "ESP32-Firmware-A1B2C3-Setup"
String password = DeviceInfo::getDefaultPassword();    // "ESP32-Firmware-A1B2C3" or custom
wifi.setAPName(apName.c_str());
wifi.setAPPassword(password.c_str());
```

**Class Interface:**
```cpp
class WiFiManagerFeature : public Feature {
public:
    WiFiManagerFeature(const char* apName, const char* apPassword, uint16_t configPortalTimeout);
    void setup() override;
    void loop() override;
    const char* getName() const override { return "WiFiManager"; }
    
    void setAPName(const char* name);      // NEW: Set AP name dynamically
    void setAPPassword(const char* pass);   // NEW: Set AP password dynamically
    
    bool isConnected() const;
    String getIPAddress() const;
};
```

### 2. LoggingFeature

**Purpose:** Centralized logging with serial output, parallel syslog output, and configurable boot/runtime log levels.

**Library:** None (custom implementation)

**Features:**
- Separate log levels for serial and syslog outputs
- Boot phase with higher verbosity (5 minutes default), transitioning to runtime level
- Parallel UDP syslog output (RFC 3164 BSD format)
- Timestamp support with NTP time or millis() fallback
- Firmware name in syslog app field

**Constructor Parameters:**
- `uint32_t baudRate` - Serial baud rate
- `uint8_t serialBootLogLevel` - Log level for serial during boot phase
- `uint8_t serialRuntimeLogLevel` - Log level for serial after boot phase ends
- `uint32_t bootDurationMs` - Duration of boot phase in milliseconds (300000 = 5 min)
- `uint8_t syslogLogLevel` - Log level for syslog output
- `const char* syslogServer` - Syslog server hostname/IP (empty string = disabled)
- `uint16_t syslogPort` - Syslog server UDP port
- `const char* hostname` - Device hostname for syslog messages (set dynamically)
- `bool enableTimestamp` - Include timestamp in log messages

**Build Flags:**
```ini
log_serial_boot_level = 4
log_serial_runtime_level = 3
log_boot_duration_ms = 300000
log_syslog_level = 3
syslog_server = 
```

**Syslog Format:**
```
<PRI>timestamp hostname FIRMWARE_NAME: message
```

Example: `<134>Jan 15 10:23:45 esp32-firmware-a1b2c3 ESP32-Firmware: WiFi connected`

**Dynamic Configuration:**
```cpp
// In main.cpp setup()
String hostname = DeviceInfo::getHostname();  // "esp32-firmware-a1b2c3"
logger.setHostname(hostname.c_str());
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
    void loop() override;
    const char* getName() const override { return "Logging"; }
    
    void setHostname(const char* hostname);  // NEW: Set hostname dynamically
    
    void error(const char* format, ...);
    void warn(const char* format, ...);
    void info(const char* format, ...);
    void debug(const char* format, ...);
    void verbose(const char* format, ...);
    
    static LoggingFeature* getInstance();
    
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

**Build Flags:**
```ini
ntp_server1 = de.pool.ntp.org
ntp_server2 = europe.pool.ntp.org
timezone = CET-1CEST,M3.5.0,M10.5.0/3
```

**Constructor Parameters:**
- `const char* ntpServer1` - Primary NTP server (EU/DE pool)
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

**Purpose:** Async web server for REST API and web interface with dynamic device identification.

**Library:** `me-no-dev/ESPAsyncWebServer` + `me-no-dev/AsyncTCP`

**Constructor Parameters:**
- `uint16_t port` - HTTP server port
- `const char* username` - Basic auth username (empty = no auth)
- `const char* password` - Basic auth password (set dynamically from DeviceInfo)

**Build Flags:**
```ini
webserver_port = 80
webserver_username = admin
; Password set dynamically
```

**Dynamic Configuration:**
```cpp
// In main.cpp setup()
String password = DeviceInfo::getDefaultPassword(); // "ESP32-Firmware-A1B2C3" or custom
webServer.setPassword(password.c_str());
```

**Web Interface:**
- Page title shows device identity: `{FirmwareName} {DeviceID}`
- Example: "ESP32-Firmware ESP32-Firmware-A1B2C3"
- Accessible via mDNS: `http://{hostname}.local/`

**Class Interface:**
```cpp
class WebServerFeature : public Feature {
public:
    WebServerFeature(uint16_t port, const char* username, const char* password);
    void setup() override;
    const char* getName() const override { return "WebServer"; }
    
    void setPassword(const char* password);  // NEW: Set password dynamically
    
    AsyncWebServer* getServer();
    void addHandler(AsyncWebHandler* handler);
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
};
```

### 5. OTAFeature

**Purpose:** Over-the-air firmware updates via ArduinoOTA.

**Library:** Built-in `ArduinoOTA`

**Constructor Parameters:**
- `const char* hostname` - mDNS hostname for OTA discovery (set dynamically)
- `const char* password` - OTA password (set dynamically from DeviceInfo)
- `uint16_t port` - OTA port (3232)

**Build Flags:**
```ini
; No hostname or password - set dynamically
ota_port = 3232
```

**Dynamic Configuration:**
```cpp
// In main.cpp setup()
String hostname = DeviceInfo::getHostname();      // "esp32-firmware-a1b2c3"
String password = DeviceInfo::getDefaultPassword(); // "ESP32-Firmware-A1B2C3" or custom
ota.setHostname(hostname.c_str());
ota.setPassword(password.c_str());
```

**Class Interface:**
```cpp
class OTAFeature : public Feature {
public:
    OTAFeature(const char* hostname, const char* password, uint16_t port);
    void setup() override;
    void loop() override;
    const char* getName() const override { return "OTA"; }
    
    void setHostname(const char* hostname);  // NEW: Set hostname dynamically
    void setPassword(const char* password);   // NEW: Set password dynamically
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

**Purpose:** Write data to InfluxDB using line protocol over HTTP with automatic device tagging.

**Supports:** Both InfluxDB 1.x (user/password) and 2.x (org/bucket/token)

**Library:** Built-in `HTTPClient`

**Build Flags in config.ini:**
```ini
influxdb_url = http://192.168.1.100:8086
influxdb_version = 1
influxdb_database =           ; Empty = use FIRMWARE_NAME as database
influxdb_username = myuser
influxdb_password = mypassword
```

**Automatic Device Tagging:**

DataCollection entries automatically include device identity tags in line protocol:

```
measurement,device_id=ESP32-Firmware-A1B2C3,firmware=ESP32-Firmware,version=1.0.0,other=tags field=value timestamp
```

Setup device ID on collections:
```cpp
// In main.cpp setup()
sensorData.setDeviceId(deviceId.c_str());
modbusData.setDeviceId(deviceId.c_str());
```

**Benefits:**
- Query by device: `WHERE "device_id" = 'ESP32-Firmware-A1B2C3'`
- Query by firmware: `WHERE "firmware" = 'ESP32-Firmware'`
- Track versions: `WHERE "version" = '1.0.0'`
- Aggregate across devices: `GROUP BY "device_id"`

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
    
    void queue(const String& lineProtocol);
    bool upload();
    bool isConnected() const;
    size_t pendingCount() const;
};
```

**DataCollection Device ID Support:**
```cpp
template<typename T, size_t MaxEntries>
class DataCollection {
public:
    void setDeviceId(const char* deviceId);  // NEW: Set device ID for tagging
    
    // entryToLineProtocol() automatically adds device_id, firmware, version tags
    String entryToLineProtocol(size_t index) const;
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

**Purpose:** MQTT client with auto-reconnect and dynamic per-device topics.

**Library:** `knolleary/PubSubClient`

**Constructor Parameters:**
- `const char* server` - MQTT broker hostname/IP
- `uint16_t port` - MQTT broker port
- `const char* username` - MQTT username (empty = no auth)
- `const char* password` - MQTT password
- `const char* clientId` - MQTT client identifier (set dynamically to hostname)
- `const char* baseTopic` - Base topic prefix (set dynamically per device)
- `uint32_t reconnectIntervalMs` - Reconnect attempt interval

**Build Flags:**
```ini
mqtt_server = 192.168.1.100
mqtt_port = 1883
mqtt_username = 
mqtt_password = 
; clientId and baseTopic set dynamically
mqtt_reconnect_interval = 5000
```

**Dynamic Configuration:**
```cpp
// In main.cpp setup()
String mqttClientId = DeviceInfo::getHostname();  // "esp32-firmware-a1b2c3"
String mqttBaseTopic = String(FIRMWARE_NAME) + "/" + hostname;  // "ESP32-Firmware/esp32-firmware-a1b2c3"
mqtt.setClientId(mqttClientId.c_str());
mqtt.setBaseTopic(mqttBaseTopic.c_str());
```

**Topic Structure:**
- Base: `{FIRMWARE_NAME}/{hostname}/`
- Example: `ESP32-Firmware/esp32-firmware-a1b2c3/sensors/state`
- Benefit: Multiple devices can coexist without topic conflicts

**Class Interface:**
```cpp
class MQTTFeature : public Feature {
public:
    MQTTFeature(const char* server, uint16_t port,
                const char* username, const char* password,
                const char* clientId, const char* baseTopic,
                uint32_t reconnectIntervalMs = 5000);
    void setup() override;
    void loop() override;
    const char* getName() const override { return "MQTT"; }
    bool isReady() const override { return _connected; }
    
    void setClientId(const char* clientId);    // NEW: Set client ID dynamically
    void setBaseTopic(const char* baseTopic);  // NEW: Set base topic dynamically
    
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

// Publish discovery using DeviceInfo for identity
DataCollectionMQTT::publishDiscovery(&mqtt, "sensors", sensorHAConfig, 3,
    DeviceInfo::getDeviceId().c_str(),      // Device name: "ESP32-Firmware-A1B2C3"
    DeviceInfo::getDeviceId().c_str(),      // Unique device ID
    "joba-1",                                // Manufacturer (GitHub username)
    DeviceInfo::getFirmwareName(),          // Model: "ESP32-Firmware"
    DeviceInfo::getFirmwareVersion());      // Version: "1.0.0"

// Publish data on each collection
DataCollectionMQTT::publishLatest(&mqtt, sensorData, "sensors");
```

**Home Assistant Device Properties:**
| Property | Source | Example |
|----------|--------|---------|
| Device Name | `DeviceInfo::getDeviceId()` | ESP32-Firmware-A1B2C3 |
| Device ID | `DeviceInfo::getDeviceId()` | ESP32-Firmware-A1B2C3 |
| Manufacturer | Hardcoded | joba-1 |
| Model | `FIRMWARE_NAME` | ESP32-Firmware |
| SW Version | `FIRMWARE_VERSION` | 1.0.0 |

### 10. LEDFeature (NEW)

**Purpose:** Visual activity indicator - LED stays on during setup, pulses briefly on sensor data activity.

**Library:** None (GPIO only)

**Constructor Parameters:**
- `uint8_t pin` - GPIO pin for LED (2 for built-in)
- `bool activeLow` - Invert logic for active-low LEDs (true for ESP32 built-in)

**Build Flags:**
```ini
led_pin = 2
led_active_low = true
```

**Behavior:**
- **During setup:** LED stays ON (solid) to indicate initialization
- **After setup:** LED turns OFF
- **On activity:** LED pulses briefly (50ms) when `pulse()` is called
- **Overflow-safe:** Uses subtraction-based timing to handle millis() overflow

**Class Interface:**
```cpp
class LEDFeature : public Feature {
public:
    LEDFeature(uint8_t pin, bool activeLow = false);
    void setup() override;  // Turns LED ON during setup
    void loop() override;   // Handles pulse timing
    const char* getName() const override { return "LED"; }
    
    void turnOn();          // Keep LED on
    void turnOff();         // Turn LED off
    void pulse(uint32_t durationMs = 50);  // Brief flash
    void endSetup();        // Call after setup complete to turn off
};
```

**Usage in main.cpp:**
```cpp
LEDFeature led(LED_PIN, LED_ACTIVE_LOW);

void setup() {
    led.setup();  // LED turns on
    
    // ... other setup ...
    
    led.endSetup();  // LED turns off when setup complete
}

void collectSensorData() {
    // ... read sensors ...
    
    led.pulse();  // Brief flash to indicate activity
}
```

**Implementation Notes:**
- Uses explicit `_isPulsing` boolean flag instead of sentinel value
- Safe across millis() overflow: `if (millis() - _pulseStart >= _pulseDuration)`
- Does NOT use: `if (millis() >= _pulseEndTime)` which fails on overflow

### 11. ModbusRTUFeature

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
modbus_serial_rx = 16
modbus_serial_tx = 17
modbus_baud_rate = 9600
modbus_de_pin = -1
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

The project uses a split configuration approach:

### platformio.ini (Build Configuration)

```ini
; platformio.ini
[platformio]
default_envs = serial

[env]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32dev
framework = arduino
monitor_speed = 115200

; Pre-build script and user configuration
extra_scripts = pre:pre_build.py
extra_configs = config.ini

; Library dependencies
lib_deps = 
    tzapu/WiFiManager@^2.0.17
    me-no-dev/ESPAsyncWebServer@^1.2.4
    me-no-dev/AsyncTCP@^1.1.1
    knolleary/PubSubClient@^2.8

; Build flags reference config.ini values
build_flags = 
    ; Firmware Identity (from config.ini)
    -D FIRMWARE_NAME=\"${user_config.firmware_name}\"
    -D FIRMWARE_VERSION=\"${user_config.firmware_version}\"
    -D DEVICE_INSTANCE=${user_config.device_instance}
    -D DEFAULT_PASSWORD=\"${user_config.default_password}\"
    
    ; Logging (from config.ini)
    -D LOG_BAUD_RATE=115200
    -D LOG_SERIAL_BOOT_LEVEL=${user_config.log_serial_boot_level}
    -D LOG_SERIAL_RUNTIME_LEVEL=${user_config.log_serial_runtime_level}
    -D LOG_BOOT_DURATION_MS=${user_config.log_boot_duration_ms}
    -D LOG_SYSLOG_LEVEL=${user_config.log_syslog_level}
    -D LOG_SYSLOG_SERVER=\"${user_config.syslog_server}\"
    
    ; Network (from config.ini)
    -D WIFI_CONFIG_PORTAL_TIMEOUT=${user_config.wifi_config_portal_timeout}
    -D NTP_SERVER1=\"${user_config.ntp_server1}\"
    -D NTP_SERVER2=\"${user_config.ntp_server2}\"
    -D TIMEZONE=\"${user_config.timezone}\"
    
    ; Services (from config.ini)
    -D INFLUXDB_URL=\"${user_config.influxdb_url}\"
    -D INFLUXDB_VERSION=${user_config.influxdb_version}
    -D INFLUXDB_DATABASE=\"${user_config.influxdb_database}\"
    -D MQTT_SERVER=\"${user_config.mqtt_server}\"
    
    ; Hardware (from config.ini)
    -D LED_PIN=${user_config.led_pin}
    -D LED_ACTIVE_LOW=${user_config.led_active_low}
    -D MODBUS_SERIAL_RX=${user_config.modbus_serial_rx}
    -D MODBUS_SERIAL_TX=${user_config.modbus_serial_tx}
    -D MODBUS_BAUD_RATE=${user_config.modbus_baud_rate}
    -D MODBUS_DE_PIN=${user_config.modbus_de_pin}

; Serial upload
[env:serial]
upload_protocol = esptool
upload_port = /dev/ttyUSB0

; OTA upload (hostname set dynamically)
[env:ota]
upload_protocol = espota
upload_port = esp32-firmware-a1b2c3.local  ; Replace with actual hostname
upload_flags = 
    --auth=${user_config.default_password}
```

### config.ini.template (User Configuration Template)

```ini
; config.ini.template - Copy to config.ini and customize
[user_config]
; ============================================
; Firmware Identity
; ============================================
firmware_name = ESP32-Firmware
firmware_version = 1.0.0
; 0 = use MAC-based device ID, >0 = manual instance number
device_instance = 0
; Empty = auto-generate from device ID
default_password = 

; ============================================
; Logging
; ============================================
log_serial_boot_level = 4
log_serial_runtime_level = 3
log_boot_duration_ms = 300000
log_syslog_level = 3
syslog_server = 

; ============================================
; Network
; ============================================
wifi_config_portal_timeout = 180
ntp_server1 = de.pool.ntp.org
ntp_server2 = europe.pool.ntp.org
timezone = CET-1CEST,M3.5.0,M10.5.0/3

; ============================================
; InfluxDB
; ============================================
influxdb_url = http://192.168.1.100:8086
influxdb_version = 1
; Empty = use firmware_name as database
influxdb_database = 
influxdb_username = 
influxdb_password = 

; ============================================
; MQTT (base topic auto-generated per device)
; ============================================
mqtt_server = 
mqtt_username = 
mqtt_password = 

; ============================================
; Hardware
; ============================================
led_pin = 2
led_active_low = true
modbus_serial_rx = 16
modbus_serial_tx = 17
modbus_baud_rate = 9600
modbus_de_pin = -1
```

## Main Application Entry Point

```cpp
// main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "OTAFeature.h"
#include "MQTTFeature.h"
#include "LEDFeature.h"

// Global device identity strings
String deviceId;
String hostname;
String apName;
String mqttClientId;
String mqttBaseTopic;
String defaultPassword;

// Feature instances (with placeholder values - set dynamically in setup)
LoggingFeature logging(LOG_BAUD_RATE, LOG_SERIAL_BOOT_LEVEL, LOG_SERIAL_RUNTIME_LEVEL,
                       LOG_BOOT_DURATION_MS, LOG_SYSLOG_LEVEL, LOG_SYSLOG_SERVER,
                       514, "", true);
WiFiManagerFeature wifiManager("", "", WIFI_CONFIG_PORTAL_TIMEOUT);
TimeSyncFeature timeSync(NTP_SERVER1, NTP_SERVER2, TIMEZONE, 3600000);
WebServerFeature webServer(WEBSERVER_PORT, WEBSERVER_USERNAME, "");
OTAFeature ota("", "", OTA_PORT);
MQTTFeature mqtt(MQTT_SERVER, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD, "", "", 5000);
LEDFeature led(LED_PIN, LED_ACTIVE_LOW);

// Array of all features
Feature* features[] = { &logging, &led, &wifiManager, &timeSync, &webServer, &ota, &mqtt };
const size_t featureCount = sizeof(features) / sizeof(features[0]);

void setup() {
    // LED on during setup
    led.setup();
    logging.setup();
    
    // Enable WiFi to get MAC address
    WiFi.mode(WIFI_STA);
    delay(100);
    
    // Generate device identity
    deviceId = DeviceInfo::getDeviceId();
    hostname = DeviceInfo::getHostname();
    apName = deviceId + "-Setup";
    mqttClientId = hostname;
    mqttBaseTopic = String(FIRMWARE_NAME) + "/" + hostname;
    defaultPassword = DeviceInfo::getDefaultPassword();
    
    LOG_I("=== %s v%s ===", FIRMWARE_NAME, FIRMWARE_VERSION);
    LOG_I("Device ID: %s", deviceId.c_str());
    LOG_I("Hostname: %s", hostname.c_str());
    LOG_I("Password: %s", defaultPassword.c_str());
    LOG_I("MQTT Topic: %s", mqttBaseTopic.c_str());
    
    // Configure features with dynamic values
    wifiManager.setAPName(apName.c_str());
    wifiManager.setAPPassword(defaultPassword.c_str());
    ota.setHostname(hostname.c_str());
    ota.setPassword(defaultPassword.c_str());
    logging.setHostname(hostname.c_str());
    mqtt.setClientId(mqttClientId.c_str());
    mqtt.setBaseTopic(mqttBaseTopic.c_str());
    webServer.setPassword(defaultPassword.c_str());
    
    // Initialize remaining features
    for (size_t i = 2; i < featureCount; i++) {
        LOG_I("Setting up: %s", features[i]->getName());
        features[i]->setup();
    }
    
    led.endSetup();  // LED off when setup complete
    LOG_I("Setup complete");
}

void loop() {
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->loop();
    }
}
```

## Configuration Summary

### Dynamic Configuration (from DeviceInfo)

| Property | Value | Source |
|----------|-------|--------|
| Device ID | `ESP32-Firmware-A1B2C3` | FIRMWARE_NAME + MAC suffix |
| Hostname | `esp32-firmware-a1b2c3` | Device ID lowercase |
| AP Name | `ESP32-Firmware-A1B2C3-Setup` | Device ID + "-Setup" |
| Password | `ESP32-Firmware-A1B2C3` | Auto-generated or custom |
| MQTT Client ID | `esp32-firmware-a1b2c3` | Hostname |
| MQTT Base Topic | `ESP32-Firmware/esp32-firmware-a1b2c3` | Firmware/hostname |

### Static Configuration (from config.ini)

| Category | Setting | Default |
|----------|---------|---------|
| Identity | `firmware_name` | `ESP32-Firmware` |
| Identity | `firmware_version` | `1.0.0` |
| Identity | `device_instance` | `0` (use MAC) |
| Logging | `log_serial_boot_level` | `4` (DEBUG) |
| Logging | `log_serial_runtime_level` | `3` (INFO) |
| Logging | `log_boot_duration_ms` | `300000` (5 min) |
| Network | `ntp_server1` | `de.pool.ntp.org` |
| Network | `ntp_server2` | `europe.pool.ntp.org` |
| Network | `timezone` | Central European |
| InfluxDB | `influxdb_database` | FIRMWARE_NAME if empty |
| Hardware | `led_pin` | `2` (built-in) |
| Hardware | `led_active_low` | `true` |

## Dependencies

| Library | Version | Purpose |
|---------|---------|----------|
| pioarduino/platform-espressif32 | stable | ESP32 platform |
| `tzapu/WiFiManager` | ^2.0.17 | WiFi configuration portal |
| `me-no-dev/ESPAsyncWebServer` | ^1.2.4 | Async HTTP server |
| `me-no-dev/AsyncTCP` | ^1.1.1 | Async TCP for ESP32 |
| `bblanchon/ArduinoJson` | ^7.0.0 | JSON serialization |
| `knolleary/PubSubClient` | ^2.8 | MQTT client |

## Notes

1. **Configuration Split:** Build-essential settings in platformio.ini, user settings in config.ini (gitignored)

2. **Auto-Configuration:** Run pre_build.py or first build to create config.ini from template

3. **Feature Initialization Order:** LoggingFeature first, LEDFeature second, WiFiManagerFeature before network features

4. **Device Identity:** Generated from MAC address after WiFi.mode(WIFI_STA) is called

5. **millis() Overflow:** LEDFeature uses subtraction-based timing to handle 49.7-day overflow correctly

4. **OTA Environment:** Ensure the device is connected to the network and reachable before using OTA upload.

5. **Serial Port:** Adjust `upload_port` in `[env:serial]` to match your system (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux).

6. **mDNS:** The OTA hostname uses mDNS, ensure your network supports it or use IP address instead.
