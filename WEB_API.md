# Web API Documentation

This firmware exposes a small HTTP API for status, storage, sensor data, and Modbus diagnostics/control.

## Base URL

- `http://<device-ip>/`
- `http://<hostname>.local/` (mDNS, if available on your network)

## Authentication

Most endpoints require HTTP Basic Auth when auth is enabled.

- Username: `admin`
- Password: the device password (by default: `{FIRMWARE_NAME}-{MAC_SUFFIX}` shown in the boot log, or the password configured in `config.ini`).

Examples below use:

```bash
curl -u admin:<password> http://<device-ip>/api/status
```

Notes:
- `/health` is intentionally unauthenticated.
- If auth is enabled and you omit credentials, you’ll get an HTTP 401 challenge.

## Response Format

- Most endpoints return JSON with `Content-Type: application/json`.
- The root page `/` and `/view/*` pages return HTML.

## Common HTTP Errors

- `400` – missing/invalid parameters.
- `401` – authentication required/failed.
- `409` – Modbus sending disabled (listen-only build via `MODBUS_LISTEN_ONLY`).

---

# Endpoints

## System

### GET `/health`
Health check.

```bash
curl http://<device-ip>/health
```

### GET `/api/status`
System/network status.

```bash
curl -u admin:<password> http://<device-ip>/api/status
```

### GET `/api/buildinfo`
Firmware build information.

```bash
curl -u admin:<password> http://<device-ip>/api/buildinfo
```

### POST `/api/reset`
Schedules a device restart (ESP32 reboot). This is delayed slightly so the HTTP response can be returned.

Form fields / parameters:
- `delayMs` (optional number): delay before restart (clamped to 50..10000ms). Default: 250ms.

```bash
curl -u admin:<password> \
  -X POST 'http://<device-ip>/api/reset' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'delayMs=250'
```

## Storage

### GET `/api/storage`
Storage diagnostics.

```bash
curl -u admin:<password> http://<device-ip>/api/storage
```

### GET `/api/storage/list?path=/...`
List directory contents.

Parameters:
- `path` (string): directory path (example: `/`, `/data`, `/modbus`)

```bash
curl -u admin:<password> 'http://<device-ip>/api/storage/list?path=/'
```

### GET `/api/storage/file?path=/...`
Download a file.

Parameters:
- `path` (string): file path (example: `/data/sensors.json`)

```bash
curl -u admin:<password> -OJ 'http://<device-ip>/api/storage/file?path=/data/sensors.json'
```

## Data Collections

### GET `/api/sensors`
All stored sensor entries as JSON.

```bash
curl -u admin:<password> http://<device-ip>/api/sensors
```

### GET `/api/sensors/latest`
Latest sensor entry.

```bash
curl -u admin:<password> http://<device-ip>/api/sensors/latest
```

## Modbus

These endpoints expose diagnostics plus helper calls to queue reads/writes.

### GET `/api/modbus/status`
Modbus RTU runtime status and counters.

```bash
curl -u admin:<password> http://<device-ip>/api/modbus/status
```

### GET `/api/modbus/devices`
List configured Modbus units, their type, and cached value counts.

```bash
curl -u admin:<password> http://<device-ip>/api/modbus/devices
```

### GET `/api/modbus/device?unit=<id>[&meta=1]`
Get cached values for one unit.

Parameters:
- `unit` (integer, required): Modbus unit ID
- `meta` (optional): if present (any value), returns a lightweight response with counts/type only

Examples:

```bash
curl -u admin:<password> 'http://<device-ip>/api/modbus/device?unit=3'
curl -u admin:<password> 'http://<device-ip>/api/modbus/device?unit=3&meta=1'
```

### GET `/api/modbus/read?unit=<id>&register=<name>`
Queues a read for a named register and returns the *currently cached* value.

Parameters:
- `unit` (integer, required): Modbus unit ID
- `register` (string, required): register name as defined by the device type JSON

```bash
curl -u admin:<password> 'http://<device-ip>/api/modbus/read?unit=3&register=grid_voltage'
```

### POST `/api/modbus/write`
Queues a write for a named register.

Form fields (required):
- `unit` (integer)
- `register` (string)
- `value` (number)

```bash
curl -u admin:<password> \
  -X POST 'http://<device-ip>/api/modbus/write' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'unit=3&register=inverter_enable&value=1'
```

### GET `/api/modbus/raw/read?unit=<id>&address=<addr>&count=<n>[&fc=3]`
Queues a raw Modbus read request.

Parameters:
- `unit` (integer, required): Modbus unit ID
- `address` (integer, required): start register address
- `count` (integer, required): number of registers to read
- `fc` (integer, optional): function code (default `3`), typically `3` or `4`

```bash
curl -u admin:<password> 'http://<device-ip>/api/modbus/raw/read?unit=3&address=0&count=2&fc=3'
```

### GET `/api/modbus/maps`
Returns aggregated register maps learned/observed by bus monitoring.

```bash
curl -u admin:<password> http://<device-ip>/api/modbus/maps
```

### GET `/api/modbus/types`
Lists available device type names.

```bash
curl -u admin:<password> http://<device-ip>/api/modbus/types
```

### GET `/api/modbus/monitor`
Returns recent Modbus frames and monitoring data.

```bash
curl -u admin:<password> http://<device-ip>/api/modbus/monitor
```

---

# MQTT Commands

If MQTT is enabled and connected, the device subscribes to:

- `<baseTopic>/cmd/reset`
- `<baseTopic>/cmd/restart`

Accepted payloads (case-insensitive, trimmed): `1`, `true`, `reset`, `restart`, `reboot`.

When accepted, the device schedules a restart (~250ms delay) and publishes an acknowledgement to:

- `<baseTopic>/status/reset` with payload `scheduled` or `already_scheduled`
