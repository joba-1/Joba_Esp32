# Feature Architecture

## Overview

The firmware uses a modular **Feature** pattern defined in `src/Feature.h`.
Each feature implements four methods:

| Method        | Contract                                                    |
|---------------|-------------------------------------------------------------|
| `setup()`     | Called once during boot, in array order. **Must be non-blocking.** |
| `loop()`      | Called every main-loop iteration. **Must be non-blocking.** |
| `getName()`   | Short label for logging / CPU profiling.                    |
| `isReady()`   | `true` when the feature is fully operational.               |

All features are stored in a global `features[]` array in `main.cpp`.
`setup()` iterates the array once; `loop()` iterates every cycle with
per-feature microsecond timing via `ResetDiagnostics::recordLoopDurationUs()`.

---

## Feature Inventory

Listed in **array order** (= initialization & loop execution order).
Order matters: a feature may depend on earlier features being ready.

| #  | Name (`getName()`) | Class / Files | Purpose |
|----|---------------------|---------------|---------|
| 0  | `Logging`     | `LoggingFeature` (.h/.cpp)           | Serial + Syslog logging, `LOG_*` macros |
| 1  | `LED`         | `LEDFeature` (.h)                     | Activity-indicator LED, pulse on data |
| 2  | `WiFiMgr`     | `WiFiManagerFeature` (.h/.cpp)        | WiFi STA connection, captive config portal |
| 3  | `TimeSync`    | `TimeSyncFeature` (.h/.cpp)           | NTP time synchronisation |
| 4  | `Storage`     | `StorageFeature` (.h/.cpp)            | LittleFS mount, file read/write helpers |
| 5  | `WebServer`   | `WebServerFeature` (.h/.cpp)          | ESPAsyncWebServer, Basic auth, REST API |
| 6  | `InfluxDB`    | `InfluxDBFeature` (.h/.cpp)           | Batch line-protocol upload (background task) |
| 7  | `MQTT`        | `MQTTFeature` (.h/.cpp)               | PubSubClient connection, publish/subscribe |
| 8  | `ModbusRTU`   | `ModbusRTUFeature` (.h/.cpp)          | RS485 bus monitor, TX queue, gap-aware scheduler |
| 9  | `ModbusDev`   | `ModbusDeviceFeature` (.h)            | Polls Modbus devices via `ModbusDeviceManager` |
| 10 | `SensorColl`  | `SensorCollectionFeature` (.h)        | Periodic sensor data collection + persistence |
| 11 | `MQTTInteg`   | `MQTTIntegrationFeature` (.h)         | HA autodiscovery, cmd subscriptions, state publish |

---

## Dependency Graph

```
Logging ─────────────────────────────────────────────────────┐ (used by all)
LED                                                          │
WiFiMgr ──────────┬──────────────────────────────────────────┤
                  │                                          │
TimeSync ─────────┤                                          │
                  │                                          │
Storage ──────────┼──────── ModbusDev ─── ModbusDeviceManager│
                  │              │                           │
WebServer ────────┤              │ (async handlers share     │
                  │              │  ModbusDeviceManager)      │
InfluxDB ─────────┤              │                           │
                  │              ▼                           │
MQTT ─────────────┼──── MQTTInteg (HA discovery, state pub)  │
                  │              │                           │
ModbusRTU ────────┘              │                           │
                                 ▼                           │
                          SensorColl ────────────────────────┘
```

### Per-Feature Dependencies

| Feature | Depends on (runtime) | Notes |
|---------|---------------------|-------|
| **Logging** | — | Must be first; all features call `LOG_*` |
| **LED** | — | Standalone GPIO; pulse() called from many features |
| **WiFiMgr** | — | Non-blocking config portal (`setConfigPortalBlocking(false)`) |
| **TimeSync** | WiFiMgr | Needs network for NTP |
| **Storage** | — | LittleFS, must precede features that read files |
| **WebServer** | WiFiMgr, Storage | Serves API endpoints; Basic auth; accesses LittleFS |
| **InfluxDB** | WiFiMgr | HTTP POST to InfluxDB server |
| **MQTT** | WiFiMgr | PubSubClient TCP connection |
| **ModbusRTU** | — | Hardware Serial2; uses no other features |
| **ModbusDev** | ModbusRTU, Storage | `ModbusDeviceManager` created late in `setup()` after storage is ready |
| **SensorColl** | InfluxDB, MQTT, LED | Queues to InfluxDB, publishes to MQTT, pulses LED |
| **MQTTInteg** | MQTT, ModbusDev | HA discovery + Modbus state publish; configured via `configure()` after `modbusDevices` exists |

---

## Threading Model

The firmware runs on an ESP32 dual-core SoC (240 MHz).

### Execution Contexts

| Context | Core | Priority | Description |
|---------|------|----------|-------------|
| **Arduino `loop()`** | 1 | 1 | All `Feature::loop()` calls, main application logic |
| **InfluxDB upload task** | 0 | 1 | FreeRTOS task (`uploadTaskFunc`), 4 KB stack. Sleeps 100 ms between checks; wakes to POST buffered data |
| **AsyncWebServer** | 0 or 1 | — | Runs in lwIP/async_tcp context. Handlers execute on whichever core the TCP event fires |
| **ResetManager** | — | — | Spawns one-shot FreeRTOS task for `ESP.restart()` with 250 ms delay |
| **WiFi / lwIP** | 0 | — | ESP-IDF system tasks for WiFi and TCP/IP stack |

### Data Flow Between Contexts

```
Main loop (core 1)                    InfluxDB task (core 0)
─────────────────                     ─────────────────────
  SensorColl.collect()
       │
       ▼
  influxDB.queue()
       │  appends to _buffer
       ▼
  influxDB.loop()
       │  joins _buffer → _pendingPayload
       │  (guarded by _payloadMutex)
       └──────────────────────────────► uploadTaskFunc()
                                             │  reads _pendingPayload
                                             │  (guarded by _payloadMutex)
                                             ▼
                                        HTTP POST to InfluxDB
```

```
AsyncWebServer (any core)             Main loop (core 1)
─────────────────────────              ─────────────────
  GET /api/modbus/devices
       │
       ▼
  ModbusDeviceManager methods
  (guarded by _mutex recursive)  ◄──── ModbusDeviceManager::loop()
                                        (also acquires _mutex)
```

---

## Synchronisation Primitives

| Primitive | Type | Location | Protects | Wait Policy |
|-----------|------|----------|----------|-------------|
| `_payloadMutex` | Binary semaphore | `InfluxDBFeature` | `_pendingPayload` and `_pendingLineCount` | `pdMS_TO_TICKS(100)` — non-blocking timeout |
| `_mutex` | Recursive mutex | `ModbusDeviceManager` | All device state (values, config, device list) | `portMAX_DELAY` — blocks indefinitely |

---

## Blocking Risks

### MQTT `connect()` — up to ~5 s

`PubSubClient::connect()` is synchronous TCP. When the broker is
unreachable, the call blocks until the TCP connect timeout (~5 s).
`MQTTFeature::reconnect()` rate-limits attempts via `_reconnectIntervalMs`
(default 5 s) so this blocks at most once per interval.

**Impact:** stalls the entire `loop()` for all features during the
connect attempt.

### ModbusDeviceManager mutex — `portMAX_DELAY`

`ScopedLock` acquires the recursive mutex with `portMAX_DELAY`.
If an `AsyncWebServer` handler holds the lock while the main loop tries
to acquire it (or vice versa), the caller blocks indefinitely until the
other context releases.

**In practice this is safe:** all critical sections are short (read
device state, serialize JSON) and the recursive mutex allows the same
task to re-enter.

### InfluxDB uploads — mitigated

Previously the synchronous HTTP POST blocked the main loop for 2–8 s.
This is now offloaded to a FreeRTOS background task on core 0.
`loop()` only acquires `_payloadMutex` briefly (100 ms timeout) to hand
off the payload string.

---

## Deadlock Analysis

### Classical Deadlock (Lock Ordering)

There are only two mutexes in the system:
1. `_payloadMutex` (InfluxDB) — acquired by main loop and InfluxDB task
2. `_mutex` (ModbusDeviceManager) — acquired by main loop and async web handlers

**No code path acquires both mutexes**, so classical AB/BA deadlock is
impossible.

### Single-Mutex Starvation

| Mutex | Risk | Assessment |
|-------|------|------------|
| `_payloadMutex` | InfluxDB task holds while POSTing (seconds) | **No risk.** Main loop uses a 100 ms timeout; if it can't acquire, it simply skips the handoff and retries next cycle. |
| `_mutex` | Async web handler holds while serializing large JSON | **Low risk.** The recursive mutex allows re-entrant locking from the same task. Web handlers hold it briefly. `portMAX_DELAY` could stall the main loop if a handler takes very long, but all current handlers are short. |

### Priority Inversion

Both the main loop task and the InfluxDB task run at priority 1.
Classic priority inversion (low-priority task holds lock needed by
high-priority task) does not apply. The `_mutex` in ModbusDeviceManager
is used between the main loop and async TCP callbacks which run at
system priority — FreeRTOS recursive mutexes support priority
inheritance, limiting inversion duration.

---

## Known Thread-Safety Concerns

### 1. LittleFS Concurrent Access (Medium Risk)

`AsyncWebServer` handlers and the main loop both access LittleFS
(e.g., reading Modbus JSON files, serving static files, persistence
writes). LittleFS is **not thread-safe** and there is no filesystem
mutex. In practice, web file reads and main-loop writes rarely overlap,
but a race is theoretically possible.

**Mitigation:** Modbus JSON files are read-only after boot. Persistence
writes (`DataCollection::loop()`) are infrequent. Risk is low but not
zero.

### 2. LoggingFeature Not Thread-Safe (Low Risk)

`LOG_*` macros are called from both the main loop (core 1) and
potentially from the InfluxDB background task (core 0) if InfluxDB code
logs errors. `Serial.print()` on ESP32 is interrupt-safe but not
guaranteed atomic for multi-part log lines — interleaved output is
possible.

**Mitigation:** The InfluxDB task logs infrequently (only on upload
errors). Interleaved serial output is cosmetic, not functional.

### 3. MQTT Publish from Callbacks (Low Risk)

`ModbusDeviceManager::onValueChange` callback publishes to MQTT from
the main loop. If an async web handler were to also publish MQTT
simultaneously, `PubSubClient` (single-threaded) could corrupt state.
Currently only the main loop publishes.

---

## Adding a New Feature

1. Create a class inheriting `Feature` in `src/`.
2. Implement `setup()`, `loop()`, `getName()`, optionally `isReady()`.
3. Add an instance in `main.cpp` and append to the `features[]` array
   **after** any features it depends on.
4. Ensure `loop()` is non-blocking: use `millis()` timers, state
   machines, or early-return guards — never `delay()` or busy-wait.
5. If the feature runs a background FreeRTOS task, document the mutex
   and data-flow in this file.
