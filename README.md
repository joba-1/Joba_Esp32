# ESP32 Firmware

A production-ready, modular ESP32 firmware framework for IoT data collection and home automation. Built with PlatformIO and Arduino framework, featuring automatic device identification, comprehensive data logging, and seamless Home Assistant integration.

## What It Does

This firmware transforms your ESP32 into a smart data collection gateway with:

- **Automatic Device Identity** - Unique device IDs based on MAC address, dynamic hostnames, and unified passwords
- **Zero-Touch Configuration** - WiFi captive portal, automatic time sync, and self-configuring services
- **Comprehensive Data Collection** - Typed data structures with automatic persistence and web visualization
- **Time-Series Logging** - Batch uploads to InfluxDB with automatic device tagging
- **Home Automation Ready** - MQTT with Home Assistant autodiscovery for sensors and Modbus devices
- **Industrial I/O** - Modbus RTU support with automatic register discovery and device polling
- **Remote Management** - Web interface, REST API, OTA updates, and syslog integration
- **Visual Feedback** - Built-in LED indicator for setup and data activity

## Key Features

### Core Infrastructure
- **WiFiManager** - Captive portal (`{DeviceID}-Config`) for initial WiFi setup
- **Device Identity** - Dynamic hostname, MQTT topics, and passwords based on firmware name + MAC address
- **Logging** - Centralized logging with configurable levels, serial output, and remote syslog
- **Time Sync** - NTP synchronization with EU/DE regional servers
- **Web Server** - Async HTTP server with device-specific titles and REST API
- **OTA Updates** - Over-the-air firmware updates via mDNS (`{hostname}.local`)
- **Storage** - LittleFS filesystem for configuration and data persistence
- **LED Indicator** - Visual feedback during setup and data collection (GPIO 2)

### Data Management
- **Data Collections** - Type-safe data structures with JSON/InfluxDB serialization
- **InfluxDB Integration** - Batch uploads with device_id, firmware, and version tags
- **MQTT Client** - Per-device topics (`{firmware}/{hostname}/*`) with auto-reconnect
- **Home Assistant** - Automatic discovery for all sensors and Modbus devices
- **Web Visualization** - Live data tables with auto-refresh

### Modbus RTU Support
- **Bus Monitoring** - Low-level packet capture and analysis
- **Device Polling** - Automatic register reading with configurable intervals
- **Hot Discovery** - Automatic register scanning for unknown devices
- **JSON Configuration** - Filesystem-based device definitions
- **Multi-Device** - Support for multiple devices on same bus
- **InfluxDB Logging** - Automatic logging of all Modbus register values
- **HA Integration** - Automatic Home Assistant discovery for each register

All features are non-blocking and run concurrently via a modular architecture.

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

After flashing, the ESP32 starts a WiFi access point with a unique name:

1. Connect to WiFi network **{FirmwareName}-{DeviceID}-Config** (e.g., `ESP32-Firmware-A1B2C3-Config`)
2. Password is auto-generated from firmware name + MAC (or set via `DEFAULT_PASSWORD`)
3. A captive portal should open automatically, or navigate to `192.168.4.1`
4. Select your WiFi network and enter the password
5. The ESP32 reboots and connects to your network

**Device Identity:**
- Device ID: `{FIRMWARE_NAME}-{MAC_SUFFIX}` (e.g., `ESP32-Firmware-A1B2C3`)
- Hostname: `{firmware-name}-{mac-suffix}` (e.g., `esp32-firmware-a1b2c3`)
- mDNS: `{hostname}.local` (e.g., `esp32-firmware-a1b2c3.local`)
- Default Password: `{FIRMWARE_NAME}-{MAC_SUFFIX}` (or custom via `DEFAULT_PASSWORD`)

## OTA Updates

Once the ESP32 is connected to your network:

1. **Find the device**:
   ```bash
   # Using mDNS (hostname from serial output or logs)
   ping esp32-firmware-a1b2c3.local
   
   # Or find IP from router's DHCP client list
   ```

2. **Update the OTA environment** in `platformio.ini`:
   ```ini
   [env:ota]
   upload_port = esp32-firmware-a1b2c3.local
   ; Or use IP address:
   ; upload_port = 192.168.1.xxx
   ```

3. **Upload via OTA**:
   ```bash
   pio run -e ota -t upload
   ```

## Configuration Overview

Configuration is split between build-time and runtime settings:

### config.ini (User-Specific Settings)

Create from template and customize for your environment:
```bash
python3 pre_build.py
nano config.ini
```

**Firmware Identity:**
- `firmware_name`: Your project name (used in device ID, MQTT topics, InfluxDB)
- `firmware_version`: Semantic version
- `device_instance`: 0 = use MAC-based ID, >0 = manual instance number
- `default_password`: Leave empty for auto-generated, or set custom

**Logging:**
- `log_serial_boot_level`: Serial verbosity during boot (0=OFF, 4=DEBUG)
- `log_serial_runtime_level`: Serial verbosity after boot (0=OFF, 3=INFO)
- `log_boot_duration_ms`: Time to keep boot log level (default: 300000 = 5 minutes)
- `log_syslog_level`: Syslog verbosity (0=OFF, 3=INFO)
- `syslog_server`: Remote syslog server IP (empty = disabled)

**Network:**
- `wifi_config_portal_timeout`: Seconds before portal closes (180)
- `ntp_server1`, `ntp_server2`: NTP servers (de.pool.ntp.org, europe.pool.ntp.org)
- `timezone`: POSIX timezone string (CET-1CEST,M3.5.0,M10.5.0/3)

**InfluxDB:**
- `influxdb_url`: Server URL (http://192.168.1.100:8086)
- `influxdb_version`: 1 or 2
- `influxdb_database`: Database/bucket name (empty = use firmware name)
- Authentication: username/password (v1) or org/token (v2)

**MQTT:**
- `mqtt_server`: Broker IP or hostname
- `mqtt_username`, `mqtt_password`: Optional authentication
- **Base topic automatically set to:** `{firmware_name}/{hostname}`

**Modbus RTU:**
- `modbus_serial_rx`, `modbus_serial_tx`: GPIO pins
- `modbus_baud_rate`: Communication speed (9600)
- `modbus_de_pin`: RS485 direction control (-1 if not used)

**LED Indicator:**
- `led_pin`: GPIO pin for status LED (2 = built-in)
- `led_active_low`: true for active-low logic (built-in ESP32 LED)

## Web Interface

Access the web interface at `http://<device-ip>/` or `http://{hostname}.local/` (e.g., `http://esp32-firmware-a1b2c3.local/`)

Default password: Auto-generated as `{FIRMWARE_NAME}-{MAC_SUFFIX}` (shown in serial log at boot), or your custom password from config.ini.

**Endpoints:**
- `/` - Status page with firmware info and device identity
- `/api/status` - JSON status
- `/api/<collection>` - JSON data for a data collection
- `/api/<collection>/latest` - Latest JSON entry
- `/view/<collection>` - HTML table view with auto-refresh
- `/api/storage` - Storage diagnostics (requires auth when enabled)
- `/api/storage/list?path=/foo` - List directory contents for `path` (requires auth when enabled)
- `/api/storage/file?path=/foo/bar.txt` - Download a file (requires auth when enabled)
- `/view/storage` - HTML file browser (requires auth when enabled)
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

The firmware supports MQTT with automatic Home Assistant autodiscovery. Each device gets unique MQTT topics based on its device identity.

### Dynamic MQTT Topics

MQTT topics are automatically generated per device:
- **Base topic format**: `{firmware_name}/{hostname}/`
- **Example**: `ESP32-Firmware/esp32-firmware-a1b2c3/`
- **Client ID**: `{hostname}` (unique per device)

This ensures multiple devices can coexist on the same broker without conflicts.

### Enabling MQTT

Configure MQTT broker in config.ini:

```ini
mqtt_server = 192.168.1.100
mqtt_username = mqtt_user
mqtt_password = mqtt_pass
```

The base topic is automatically set. Do not configure `MQTT_BASE_TOPIC` manually.

### Home Assistant Autodiscovery

Sensors are automatically discovered with device identity information:

```cpp
#include "DataCollectionMQTT.h"

// Define Home Assistant sensor mapping
const HASensorConfig sensorHAConfig[] = {
    { "temperature", "Temperature", HADeviceClass::TEMPERATURE, "°C", nullptr },
    { "humidity", "Humidity", HADeviceClass::HUMIDITY, "%", nullptr },
    { "rssi", "WiFi Signal", HADeviceClass::SIGNAL_STRENGTH, "dBm", nullptr },
};

// Publish autodiscovery (once when MQTT connects)
// Device info is automatically pulled from DeviceInfo
DataCollectionMQTT::publishDiscovery(
    &mqtt, "sensors", sensorHAConfig, 3,
    DeviceInfo::getDeviceId().c_str(),     // Device name: "ESP32-Firmware-A1B2C3"
    DeviceInfo::getDeviceId().c_str(),     // Unique device ID
    "joba-1",                               // Manufacturer (GitHub username)
    DeviceInfo::getFirmwareName().c_str(), // Model
    DeviceInfo::getFirmwareVersion().c_str() // Software version
);

// Publish data updates (on each reading)
DataCollectionMQTT::publishLatest(&mqtt, sensorData, "sensors");
```

### MQTT Topics

All topics are prefixed with `{firmware_name}/{hostname}/`:

| Topic | Purpose |
|-------|---------|
| `homeassistant/sensor/{deviceId}_{field}/config` | Autodiscovery (retained) |
| `{base_topic}/sensors/state` | Latest sensor values as JSON |
| `{base_topic}/status` | Device availability (online/offline) |
| `{base_topic}/modbus/unit_N/state` | Modbus device state JSON |
| `{base_topic}/modbus/unit_N/{register}` | Individual register value |

### Available Device Classes

Use these constants from `HADeviceClass` namespace:
- `TEMPERATURE`, `HUMIDITY`, `PRESSURE`
- `BATTERY`, `VOLTAGE`, `CURRENT`, `POWER`, `ENERGY`
- `SIGNAL_STRENGTH`, `ILLUMINANCE`
- `CO2`, `PM25`, `PM10`
- `TIMESTAMP`, `DURATION`

## Modbus Integration with InfluxDB & Home Assistant

Modbus device values are automatically integrated with InfluxDB and MQTT/Home Assistant when both features are enabled. All metrics include device identity tags.

### Automatic Data Flow

When a Modbus register is polled and updated:

1. **InfluxDB** - Value is queued for batch upload with tags:
   - `device_id` - Device identity (e.g., "ESP32-Firmware-A1B2C3")
   - `firmware` - Firmware name
   - `version` - Firmware version
   - `device` - Modbus device name from mapping
   - `unit_id` - Modbus unit ID
   - `register` - Register name
   - `unit` - Unit of measurement

2. **MQTT** - Value is published to device-specific topics:
   - Individual: `{firmware_name}/{hostname}/modbus/unit_N/RegisterName`
   - State JSON: `{firmware_name}/{hostname}/modbus/unit_N/state`

3. **Home Assistant** - Autodiscovery published for each register:
   - Sensors automatically appear in HA
   - Device class inferred from unit (V→voltage, W→power, etc.)
   - State class inferred from name (Energy→total_increasing, etc.)

### MQTT Topics for Modbus

All topics are prefixed with `{firmware_name}/{hostname}/`:

| Topic | Purpose |
|-------|---------|
| `homeassistant/sensor/{deviceId}_{register}/config` | Autodiscovery |
| `{base_topic}/modbus/unit_N/state` | All device values as JSON |
| `{base_topic}/modbus/unit_N/{register}` | Individual register value |
| `{base_topic}/modbus/status` | Device availability |

### InfluxDB Line Protocol Format

All metrics include device identity tags automatically:

```
modbus,device_id=ESP32-Firmware-A1B2C3,firmware=ESP32-Firmware,version=1.0.0,device=HybridInverter,unit_id=1,register=PV1Power,unit=W value=3245.5
modbus,device_id=ESP32-Firmware-A1B2C3,firmware=ESP32-Firmware,version=1.0.0,device=GridMeter,unit_id=10,register=TotalPower,unit=W value=1523.2
```

### Example: Grafana Dashboard Query

Query specific device or aggregate across devices:

```sql
-- Single device
SELECT mean("value") FROM "modbus"
WHERE "device_id" = 'ESP32-Firmware-A1B2C3' AND "register" =~ /^PV.*Power$/
GROUP BY time(5m), "register"

-- All devices with same firmware
SELECT mean("value") FROM "modbus"
WHERE "firmware" = 'ESP32-Firmware' AND "register" = 'TotalPower'
GROUP BY time(5m), "device_id"
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
