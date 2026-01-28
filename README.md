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
- **Modbus RTU** - Bus monitoring, automatic register discovery, and device polling
- **Modbus Integration** - Automatic InfluxDB logging and Home Assistant autodiscovery for Modbus devices

All features are implemented as modular, non-blocking classes derived from a common `Feature` base class.

### Supported Modbus Devices

| Device | Description | Registers |
|--------|-------------|----------|
| SDM120 | Eastron SDM120 single-phase energy meter | ~20 |
| SDM630 | Eastron SDM630 three-phase energy meter | ~60 |
| SDM72 | Eastron SDM72 three-phase energy meter | ~40 |
| SolplanetASWHybrid | Solplanet ASW 05-12k H-T2/T3 hybrid inverter | ~65 |
| SolplanetApolloSOL11H | Solplanet Apollo SOL 11H wallbox (EV charger) | ~40 |
| SolplanetAiHBG2Pro | Solplanet Ai-HB G2 Pro 150A 15.36kWh battery | ~85 |

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

2. **Configure your environment**:
   
   Create your configuration file from the template:
   ```bash
   python3 pre_build.py
   ```
   
   This creates `config.ini` from `config.ini.template` if it doesn't exist. Edit `config.ini` to customize:
   - Firmware name and version
   - Device instance number (or use MAC-based automatic ID)
   - Default password (or use auto-generated)
   - Log levels and boot duration
   - Network settings (WiFi, NTP servers, timezone)
   - External services (InfluxDB, MQTT, Syslog)
   - Hardware configuration (Modbus pins, LED pin)
   
   ```bash
   # Customize your configuration
   nano config.ini
   ```
   
   **Note:** `config.ini` is gitignored. Use `config.ini.template` as reference.

3. **Build the firmware**:
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

3. **Update the port** in `config.ini`:
   ```ini
   [user_config]
   upload_port_serial = /dev/ttyUSB0
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
| `INFLUXDB_VERSION` | 1 | InfluxDB version (1 or 2) |
| `INFLUXDB_DATABASE` | "" | Database name (V1) |
| `INFLUXDB_USERNAME` | "" | Username (V1) |
| `INFLUXDB_PASSWORD` | "" | Password (V1) |
| `INFLUXDB_ORG` | "" | Organization (V2) |
| `INFLUXDB_BUCKET` | "" | Bucket (V2) |
| `INFLUXDB_TOKEN` | "" | API token (V2) |
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

### Enabling InfluxDB

The firmware supports both InfluxDB 1.x and 2.x.

**InfluxDB 1.x** (user/password authentication):
```ini
build_flags =
    -D INFLUXDB_URL=\"http://192.168.1.100:8086\"
    -D INFLUXDB_VERSION=1
    -D INFLUXDB_DATABASE=\"sensors\"
    -D INFLUXDB_USERNAME=\"myuser\"
    -D INFLUXDB_PASSWORD=\"mypassword\"
    -D INFLUXDB_RP=\"autogen\"           ; Optional: retention policy
```

**InfluxDB 2.x** (token authentication):
```ini
build_flags =
    -D INFLUXDB_URL=\"http://192.168.1.100:8086\"
    -D INFLUXDB_VERSION=2
    -D INFLUXDB_ORG=\"my-org\"
    -D INFLUXDB_BUCKET=\"my-bucket\"
    -D INFLUXDB_TOKEN=\"my-api-token\"
```

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

## Modbus Integration with InfluxDB & Home Assistant

Modbus device values are automatically integrated with InfluxDB and MQTT/Home Assistant when both features are enabled.

### Automatic Data Flow

When a Modbus register is polled and updated:

1. **InfluxDB** - Value is queued for batch upload with tags:
   - `device` - Device name from mapping
   - `unit_id` - Modbus unit ID
   - `register` - Register name
   - `unit` - Unit of measurement

2. **MQTT** - Value is published to:
   - Individual topic: `<base_topic>/modbus/unit_N/RegisterName`
   - State topic: `<base_topic>/modbus/unit_N/state` (JSON with all values)

3. **Home Assistant** - Autodiscovery published for each register:
   - Sensors automatically appear in HA
   - Device class inferred from unit (V→voltage, W→power, etc.)
   - State class inferred from name (Energy→total_increasing, etc.)

### MQTT Topics for Modbus

| Topic | Purpose |
|-------|---------|
| `homeassistant/sensor/<device_id>_<register>/config` | Autodiscovery |
| `<base_topic>/modbus/unit_N/state` | All device values as JSON |
| `<base_topic>/modbus/unit_N/<register>` | Individual register value |
| `<base_topic>/modbus/status` | Device availability |

### InfluxDB Line Protocol Format

```
modbus,device=HybridInverter,unit_id=1,register=PV1Power,unit=W value=3245.5000
modbus,device=GridMeter,unit_id=10,register=TotalPower,unit=W value=1523.2500
```

### Example: Grafana Dashboard Query

```sql
SELECT mean("value") FROM "modbus"
WHERE "device" = 'HybridInverter' AND "register" =~ /^PV.*Power$/
GROUP BY time(5m), "register"
```

### Device Class Inference

The integration automatically infers Home Assistant device classes:

| Unit | Device Class |
|------|--------------|
| °C, °F | temperature |
| V, mV | voltage |
| A, mA | current |
| W, kW | power |
| Wh, kWh | energy |
| Hz | frequency |
| VA, kVA | apparent_power |
| VAr, kVAr | reactive_power |

Registers with "Energy", "Total", "Import", or "Export" in the name are set to `state_class: total_increasing`.

## Data Filesystem Management

The `data/` folder contains configuration files that are uploaded to the ESP32's LittleFS filesystem. This includes Modbus device definitions and mappings.

### Uploading the Filesystem

After modifying any files in the `data/` folder, upload them to the device:

**Via Serial:**
```bash
pio run -e serial -t uploadfs
```

**Via OTA:**
```bash
pio run -e ota -t uploadfs
```

> **Note:** Filesystem upload erases and replaces all files. Make sure all needed files are in the `data/` folder before uploading.

### Modbus Device Configuration

Device definitions are stored in `data/modbus/devices/` as JSON files. The device mapping in `data/modbus/devices.json` assigns unit IDs to device types.

#### Adding a New Device

1. **Create the device definition** (if the type doesn't exist):
   ```bash
   # Create a new device type file
   touch data/modbus/devices/my_device.json
   ```

   Example structure:
   ```json
   {
       "name": "MyDevice",
       "description": "Custom Modbus device",
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

2. **Add the device to the mapping** (`data/modbus/devices.json`):
   ```json
   {
       "devices": [
           {"unitId": 1, "type": "MyDevice", "name": "My Sensor"}
       ]
   }
   ```

3. **Upload the filesystem:**
   ```bash
   pio run -e serial -t uploadfs
   ```

4. **Reboot the ESP32** to load the new configuration.

#### Removing a Device

1. Remove the entry from `data/modbus/devices.json`
2. Optionally delete the device type file from `data/modbus/devices/`
3. Upload the filesystem: `pio run -e serial -t uploadfs`

#### Register Definition Fields

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Register name (used in API) |
| `address` | integer | Modbus register address (0-based) |
| `length` | integer | Number of registers (1 for 16-bit, 2 for 32-bit) |
| `functionCode` | integer | 3 = holding registers, 4 = input registers |
| `dataType` | string | See supported types below |
| `factor` | float | Multiply raw value by this factor |
| `offset` | float | Add to value after factor |
| `unit` | string | Unit of measurement |
| `pollInterval` | integer | Polling interval in milliseconds |

#### Supported Data Types

| Type | Description |
|------|-------------|
| `uint16` | Unsigned 16-bit integer |
| `int16` | Signed 16-bit integer |
| `uint32_be` | Unsigned 32-bit, big-endian |
| `uint32_le` | Unsigned 32-bit, little-endian |
| `int32_be` | Signed 32-bit, big-endian |
| `int32_le` | Signed 32-bit, little-endian |
| `float32_be` | 32-bit float, big-endian (common) |
| `float32_le` | 32-bit float, little-endian |
| `bool` | Boolean (0 or 1) |

## License

MIT
