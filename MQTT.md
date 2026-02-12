# MQTT Communication

This document describes MQTT topics and example payloads for commands supported by the firmware, including Modbus command topics and expected ack/resp topics.

Base topic
- Each device publishes/subscribes under a base topic formed as:
  `{firmware_name}/{hostname}` (the firmware `mqtt` feature sets this as the base topic).

Subscribed command topics (examples)
- `<baseTopic>/cmd/reset` — reboot command
- `<baseTopic>/cmd/restart` — reboot alias

- Modbus command topics:
  - `<baseTopic>/modbus/cmd/raw/read` — Queue a raw Modbus read (JSON payload).
  - `<baseTopic>/modbus/cmd/raw/write` — Queue a raw Modbus write (single or multiple registers, JSON payload).
  - `<baseTopic>/modbus/cmd/write` — Queue a decoded write by register name (JSON payload).

Acknowledgement / response topics
- The firmware publishes ACKs to `modbus/ack/...` and responses to `modbus/resp/...` under the same base topic. Example topics:
  - `<baseTopic>/modbus/ack/raw/read`
  - `<baseTopic>/modbus/resp/raw/read`
  - `<baseTopic>/modbus/ack/raw/write`
  - `<baseTopic>/modbus/resp/raw/write`
  - `<baseTopic>/modbus/ack/write`
  - `<baseTopic>/modbus/resp/write`

Listen-only builds
- If the firmware is built/configured with `MODBUS_LISTEN_ONLY` enabled, command handlers will acknowledge with an error and not queue/send requests.

Example `mosquitto_pub` and `mosquitto_sub` usage

Replace `<broker>` and `<baseTopic>` in the examples below.

Subscribe to ack/resp topics:

```bash
mosquitto_sub -h <broker> -t "<baseTopic>/modbus/#" -v
```

Raw read (FC3): queue a holding-register read

```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/raw/read" \
  -m '{"id":"r1","unit":1,"address":0,"count":2,"fc":3}'
```

Raw write (single register, FC6): write value 1234 to address 10

```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/raw/write" \
  -m '{"id":"w1","unit":1,"address":10,"value":1234,"fc":6}'
```

Raw write (multiple registers, FC16): write [100,200] starting at address 20

```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/raw/write" \
  -m '{"id":"w2","unit":1,"address":20,"values":[100,200],"fc":16}'
```

Decoded write by register name (uses configured device map)

```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/write" \
  -m '{"id":"dw1","unit":1,"register":"inverter_enable","value":1.0}'
```

Reset command via MQTT

```bash
# Reboot device via MQTT
mosquitto_pub -h <broker> -t "<baseTopic>/cmd/reset" -m 'reboot'
```

Notes
- All command payloads are JSON for Modbus commands (see examples). Each command typically includes an `id` string which is echoed back in ack/resp messages to correlate requests and responses.
- The ack message indicates whether the command was queued; the resp message indicates success/failure of the actual Modbus operation.
- Topics are published under the device base topic (see `main.cpp` where `mqttBaseTopic` is set).

Accepted payloads for the simple reboot topics
- The restart/reboot topics accept simple payloads (case-insensitive, trimmed): `1`, `true`, `reset`, `restart`, `reboot`.

Reset acknowledgement topic
- When a reboot is accepted, the device publishes an acknowledgement to `<baseTopic>/status/reset` with payload `scheduled` or `already_scheduled`.

Listing known devices and registers

You can request a list of known devices and the register definitions for a device via MQTT.

1) List known devices

Topic: `<baseTopic>/modbus/cmd/list_devices`
Response: published to `<baseTopic>/modbus/resp/list_devices` as a JSON array of objects with `unitId`, `deviceName`, `deviceType`, `successCount`, `errorCount`.

Example:
```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/list_devices" -m '{}'
mosquitto_sub -h <broker> -t "<baseTopic>/modbus/resp/list_devices" -v
```

2) List registers for a device

Topic: `<baseTopic>/modbus/cmd/list_registers`
Payload: JSON: `{"id":"opt","unit":<unitId>}`
Response: published to `<baseTopic>/modbus/resp/list_registers` with JSON including the same `id` and an array `registers` where each entry has `name`, `address`, `length`, `functionCode`, `unit`.

Example:
```bash
mosquitto_pub -h <broker> -t "<baseTopic>/modbus/cmd/list_registers" \
  -m '{"id":"l1","unit":1}'
mosquitto_sub -h <broker> -t "<baseTopic>/modbus/resp/list_registers" -v
```

*** End of MQTT.md
