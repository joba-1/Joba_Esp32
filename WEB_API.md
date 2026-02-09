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

Notes:
- The response includes both **firmware** build info (compiled into the binary) and a **filesystem manifest** loaded from `/build_info.json`.
- If `firmwareFilesystemMismatch` is `true`, you likely OTA-uploaded only the firmware and not the filesystem image. Fix by uploading the filesystem too:
  - `pio run -e ota -t uploadfs` (OTA)
  - `pio run -e serial -t uploadfs` (USB/serial)
- `firmware.ota.running` / `firmware.ota.boot` show which OTA slot is currently running vs selected for boot.

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

### GET `/api/modbus/crc[?limit=<n>]`
Returns recent CRC error contexts with hex dumps for the bad frame plus the frame immediately before and after (when available).

Notes:
- Each frame includes `frameType` (`request` or `response`).
- `crcReceivedHex` is the CRC that was present on the wire.
- `crcCalculatedHex` is the CRC computed from the frame bytes (unit + function + payload).
- If `isValid` is `false`, you will get `invalidReason=crc_mismatch` (and a human-readable `invalidWhy`).

Parameters:
- `limit` (integer, optional): max items to return (default `10`, max `50`)

```bash
curl --anyauth -u admin:<password> 'http://<device-ip>/api/modbus/crc?limit=10'
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

### GET `/api/modbus/patterns`

Bus pattern analysis. Reports byte-level bus statistics, transaction round-trip times,
inter-frame gap histogram (measured at the raw byte/silence-boundary level in microseconds),
per-register-range polling intervals, and detected polling cycles of the other master(s).

```bash
curl --digest -u admin:<password> http://<device-ip>/api/modbus/patterns | python3 -m json.tool
```

**Response fields:**

| Field | Description |
|-------|-------------|
| `byteStats.totalBytes` | Total bytes received on the bus since last reset |
| `byteStats.bytesPerSec` | Average bytes/second throughput |
| `byteStats.frameBoundaries` | Number of frame boundaries detected (3.5 char-time silences) |
| `byteStats.validFrames` | Frames that passed CRC |
| `byteStats.invalidFrames` | Frames that failed CRC (first-hit only, not resync attempts) |
| `transactionTimes.count` | Number of paired request→response transactions observed |
| `transactionTimes.{minMs,maxMs,meanMs,stddevMs}` | Round-trip time statistics |
| `transactionTimes.histogram[]` | RTT distribution: `<10ms` to `>=1s` (8 buckets) |
| `entries[]` | Per register-range timing: `unitId`, `fc`, `startReg`, `qty`, `count`, `interval.{minMs,maxMs,meanMs,stddevMs}` |
| `gaps.histogram[]` | Inter-frame gap distribution, buckets from `<1ms` to `>=5s`. Measured at the byte level between the last byte of one frame chunk and the first byte of the next |
| `gaps.{minUs,maxUs,meanUs}` | Gap summary statistics in microseconds |
| `cyclePosition` | Current tracking position in detected cycle (-1 = not synced) |
| `cycle[]` | Detected repeating polling sequence (if >80% match rate) |
| `cycle[].gap` | Per-step gap stats: time from end of previous response to start of this request (only after cycle detection + tracking sync) |

**Gathering data:**

1. Switch to listen-only mode: set `modbus_listen_only = 1` in `config.ini`, rebuild + uploadfs
2. Reset stats for a clean collection window:
   ```bash
   curl --digest -u admin:<password> -X POST http://<device-ip>/api/modbus/patterns/reset
   ```
3. Wait at least 5-10 minutes for representative data
4. Fetch results:
   ```bash
   curl --digest -u admin:<password> http://<device-ip>/api/modbus/patterns | python3 -m json.tool
   ```

**Interpreting results:**

- `transactionTimes.meanMs` tells you how long a typical request→response takes; multiply by ~1.5 for the minimum gap needed to fit one of your own requests
- `byteStats.bytesPerSec` shows real bus throughput; at 9600-8N1 (~960 B/s max), high values mean a busy bus
- Compare `validFrames` vs `invalidFrames` to assess signal quality; a high invalid ratio suggests electrical/wiring issues
- The gap histogram shows available windows for inserting your own Modbus requests. Look at which bucket sizes have the most counts — those are the typical inter-frame silences
- `entries[].interval.meanMs` for each register range shows how often the other master polls each register block
- If a `cycle` is detected, each step shows its `gap` (time available before that step). Steps with gaps larger than your RTT are safe insertion points

### GET `/view/modbus/patterns`

Human-friendly HTML page for the bus pattern analysis data. Auto-refreshes every 10 seconds.
Shows summary cards, transaction time histogram, register polling table, gap histogram,
and detected cycle with per-step gap analysis (color-coded: green=safe, yellow=tight, red=too short).

### POST `/api/modbus/patterns/reset`

Clears all bus pattern tracking data for a fresh collection window.

```bash
curl --digest -u admin:<password> -X POST http://<device-ip>/api/modbus/patterns/reset
```

### GET `/api/modbus/gap-scheduler`

Gap-aware TX scheduler monitoring data. Returns prediction accuracy, collision stats, dynamic safety margin, and current gap prediction.

```bash
curl --digest -u admin:<password> http://<device-ip>/api/modbus/gap-scheduler | python3 -m json.tool
```

**Response fields:**

| Field | Description |
|---|---|
| `txDecisions.inGap` | Transmissions sent into a predicted gap |
| `txDecisions.fallback` | Transmissions sent via silence-based fallback (no prediction available) |
| `txDecisions.deferred` | Requests deferred because predicted gap was too small |
| `txDecisions.gapPct` | Percentage of TX that used gap prediction |
| `prediction.successRate` | Percentage of gap-predicted TX that succeeded without collision |
| `prediction.sufficient` | Predictions confirmed by successful response |
| `prediction.insufficient` | Predictions that led to a collision/timeout |
| `collisions.count` | Total detected collisions (timeout during gap window) |
| `collisions.rate` | Collision rate as percentage of gap TX |
| `margin.current` | Current dynamic safety margin (starts 20%, +5% per collision, −1% per 50 successes) |
| `margin.min` / `margin.max` | Allowed margin range (10%–60%) |
| `currentGap.valid` | Whether a gap prediction is currently available |
| `currentGap.predictedMs` | Conservative predicted gap in ms (if valid) |

### GET `/view/modbus/scheduler`

Human-readable monitoring dashboard for the gap-aware TX scheduler. Auto-refreshes every 5 seconds.

---

# MQTT Commands

If MQTT is enabled and connected, the device subscribes to:

- `<baseTopic>/cmd/reset`
- `<baseTopic>/cmd/restart`

Accepted payloads (case-insensitive, trimmed): `1`, `true`, `reset`, `restart`, `reboot`.

When accepted, the device schedules a restart (~250ms delay) and publishes an acknowledgement to:

- `<baseTopic>/status/reset` with payload `scheduled` or `already_scheduled`
