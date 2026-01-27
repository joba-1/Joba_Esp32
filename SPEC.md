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
│   ├── DataCollection.h          # Template for typed data collections
│   └── DataCollectionWeb.h       # Web endpoints for data collections
├── data/                         # SPIFFS/LittleFS web files (if needed)
└── spec.md
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

### 6. DataCollectionWeb

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

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| `tzapu/WiFiManager` | ^2.0.17 | WiFi configuration portal |
| `me-no-dev/ESPAsyncWebServer` | ^1.2.4 | Async HTTP server |
| `me-no-dev/AsyncTCP` | ^1.1.1 | Async TCP for ESP32 |

## Notes

1. **Feature Initialization Order:** LoggingFeature must be initialized first, followed by WiFiManagerFeature before any network-dependent features.

2. **String Literals in Build Flags:** Use escaped quotes `\"value\"` for string values in build flags.

3. **Boolean Build Flags:** Use `true`/`false` which will be interpreted as `1`/`0` by the preprocessor.

4. **OTA Environment:** Ensure the device is connected to the network and reachable before using OTA upload.

5. **Serial Port:** Adjust `upload_port` in `[env:serial]` to match your system (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux).

6. **mDNS:** The OTA hostname uses mDNS, ensure your network supports it or use IP address instead.
