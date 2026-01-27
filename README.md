# ESP32 Firmware

A modular ESP32 firmware built with PlatformIO and Arduino framework, featuring WiFi management, time synchronization, async web server, and OTA updates.

## Features

- **WiFiManager** - Captive portal for WiFi configuration
- **Logging** - Centralized logging with serial output, optional syslog, and boot/runtime log levels
- **Time Sync** - NTP-based time synchronization
- **Web Server** - Async HTTP server with REST API
- **OTA Updates** - Over-the-air firmware updates
- **Storage** - LittleFS filesystem for persistent data
- **InfluxDB** - Batch upload to InfluxDB 2.x time-series database
- **MQTT** - MQTT client with Home Assistant autodiscovery
- **Data Collections** - Typed data structures with JSON serialization and web views

All features are implemented as modular, non-blocking classes derived from a common `Feature` base class.

## Prerequisites

### Install PlatformIO on Linux

1. **Install Python and pip** (if not already installed):
   ```bash
   sudo apt update
   sudo apt install python3 python3-pip python3-venv
   ```

2. **Install PlatformIO Core**:
   ```bash
   curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
   python3 get-platformio.py
   ```

3. **Add PlatformIO to PATH**:
   ```bash
   echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```

4. **Install udev rules** (for USB access without sudo):
   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

5. **Add user to dialout group** (for serial port access):
   ```bash
   sudo usermod -aG dialout $USER
   ```
   Log out and log back in for the group change to take effect.

## Building

1. **Clone the repository**:
   ```bash
   git clone <repository-url>
   cd esp32-firmware
   ```

2. **Build the firmware**:
   ```bash
   pio run
   ```

   This downloads all dependencies and compiles the firmware.

## Flashing via Serial

1. **Connect your ESP32** via USB cable.

2. **Identify the serial port**:
   ```bash
   ls /dev/ttyUSB*
   # or
   ls /dev/ttyACM*
   ```

3. **Update the port** in `platformio.ini` if needed:
   ```ini
   [env:serial]
   upload_port = /dev/ttyUSB0
   ```

4. **Flash the firmware**:
   ```bash
   pio run -e serial -t upload
   ```

5. **Monitor serial output** (optional):
   ```bash
   pio device monitor
   ```
   Press `Ctrl+]` to exit the monitor.

## Initial WiFi Configuration

After flashing, the ESP32 starts a WiFi access point:

1. Connect to WiFi network **ESP32-Config** (open network)
2. A captive portal should open automatically, or navigate to `192.168.4.1`
3. Select your WiFi network and enter the password
4. The ESP32 reboots and connects to your network

## OTA Updates

Once the ESP32 is connected to your network:

1. **Find the device IP** from serial monitor or your router's DHCP client list.

2. **Ensure mDNS works** (or use IP address):
   ```bash
   ping esp32-device.local
   ```

3. **Update via OTA**:
   ```bash
   pio run -e ota -t upload
   ```

   If using IP address instead of hostname, update `platformio.ini`:
   ```ini
   [env:ota]
   upload_port = 192.168.1.xxx
   ```

## Configuration

All configuration is done via build flags in `platformio.ini`:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `WIFI_AP_NAME` | ESP32-Config | Config portal AP name |
| `WIFI_CONFIG_PORTAL_TIMEOUT` | 180 | Portal timeout (seconds) |
| `LOG_SERIAL_BOOT_LEVEL` | 4 | Serial log level during boot (0-5) |
| `LOG_SERIAL_RUNTIME_LEVEL` | 3 | Serial log level after boot (0-5) |
| `LOG_BOOT_DURATION_MS` | 30000 | Boot phase duration (ms) |
| `LOG_SYSLOG_LEVEL` | 3 | Syslog log level (0-5) |
| `LOG_SYSLOG_SERVER` | "" | Syslog server IP (empty=disabled) |
| `LOG_SYSLOG_PORT` | 514 | Syslog UDP port |
| `NTP_SERVER1` | pool.ntp.org | Primary NTP server |
| `TIMEZONE` | CET-1CEST... | POSIX timezone |
| `WEBSERVER_PORT` | 80 | HTTP server port |
| `OTA_HOSTNAME` | esp32-device | mDNS hostname |
| `OTA_PASSWORD` | otapassword | OTA update password |
| `INFLUXDB_URL` | "" | InfluxDB server URL (empty=disabled) |
| `INFLUXDB_ORG` | "" | InfluxDB organization |
| `INFLUXDB_BUCKET` | "" | InfluxDB bucket |
| `INFLUXDB_TOKEN` | "" | InfluxDB API token |
| `MQTT_SERVER` | "" | MQTT broker address (empty=disabled) |
| `MQTT_PORT` | 1883 | MQTT broker port |
| `MQTT_USERNAME` | "" | MQTT username |
| `MQTT_PASSWORD` | "" | MQTT password |
| `MQTT_BASE_TOPIC` | esp32 | Base topic for all messages |

### Enabling Syslog

To send logs to a syslog server, set the server IP in `platformio.ini`:

```ini
build_flags =
    -D LOG_SYSLOG_SERVER=\"192.168.1.100\"
```

Logs are sent via UDP in RFC 3164 BSD syslog format.

## Web Interface

Access the web interface at `http://<device-ip>/` or `http://esp32-device.local/`

Default credentials: `admin` / `admin`

**Endpoints:**
- `/` - Status page
- `/api/status` - JSON status
- `/api/<collection>` - JSON data for a data collection
- `/api/<collection>/latest` - Latest JSON entry
- `/view/<collection>` - HTML table view with auto-refresh
- `/health` - Health check (no auth)

### Data Collection Web Views

For any registered data collection, you get:
- **API endpoint**: `/api/sensors` - Full JSON array of all data
- **Latest API**: `/api/sensors/latest` - Latest entry as JSON
- **HTML View**: `/view/sensors` - Interactive table with auto-refresh

The HTML view features:
- Dark themed responsive table
- Connection status indicator
- Entry count and last update time
- Auto-refresh (configurable, default 5 seconds)
- Newest entries highlighted at top
- Automatic timestamp formatting
- Manual refresh button

#### Registering a Collection

```cpp
#include "DataCollectionWeb.h"

// In setup(), after webServer.setup():
DataCollectionWeb::registerCollection(
    webServer.getServer(),
    sensorData,           // Your DataCollection instance
    "sensors",            // Base path -> /api/sensors, /view/sensors
    5000                  // Refresh interval (ms)
);
```

## MQTT & Home Assistant

The firmware supports MQTT with automatic Home Assistant autodiscovery. When configured, sensors automatically appear in Home Assistant without manual YAML configuration.

### Enabling MQTT

Set the MQTT broker in `platformio.ini`:

```ini
build_flags =
    -D MQTT_SERVER=\"192.168.1.100\"
    -D MQTT_USERNAME=\"mqtt_user\"
    -D MQTT_PASSWORD=\"mqtt_pass\"
    -D MQTT_BASE_TOPIC=\"esp32/living_room\"
```

### Home Assistant Autodiscovery

Define sensor configurations and publish discovery:

```cpp
#include "DataCollectionMQTT.h"

// Define Home Assistant sensor mapping
const HASensorConfig sensorHAConfig[] = {
    { "temperature", "Temperature", HADeviceClass::TEMPERATURE, "°C", nullptr },
    { "humidity", "Humidity", HADeviceClass::HUMIDITY, "%", nullptr },
    { "rssi", "WiFi Signal", HADeviceClass::SIGNAL_STRENGTH, "dBm", nullptr },
};

// Publish autodiscovery (once when MQTT connects)
DataCollectionMQTT::publishDiscovery(
    &mqtt, "sensors", sensorHAConfig, 3,
    "Living Room Sensor",    // Device name in HA
    "esp32-living-room",     // Unique device ID
    "Custom",                // Manufacturer
    "ESP32 Sensor Node",     // Model
    "1.0.0"                  // Software version
);

// Publish data updates (on each reading)
DataCollectionMQTT::publishLatest(&mqtt, sensorData, "sensors");
```

### MQTT Topics

| Topic | Purpose |
|-------|---------|
| `homeassistant/sensor/<device>/.../config` | Autodiscovery (retained) |
| `<base_topic>/sensors/state` | Latest sensor values as JSON |
| `<base_topic>/status` | Device availability (online/offline) |

### Available Device Classes

Use these constants from `HADeviceClass` namespace:
- `TEMPERATURE`, `HUMIDITY`, `PRESSURE`
- `BATTERY`, `VOLTAGE`, `CURRENT`, `POWER`, `ENERGY`
- `SIGNAL_STRENGTH`, `ILLUMINANCE`
- `CO2`, `PM25`, `PM10`
- `TIMESTAMP`, `DURATION`

## License

MIT
