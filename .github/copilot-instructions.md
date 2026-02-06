---
applyTo: "**"
---
# Joba_Esp32 (ESP32 + Arduino + PlatformIO)

- Build is PlatformIO (`platformio.ini`) with envs `serial` (USB, usually not connected) and `ota` (espota). `pio` isn’t on PATH, use `/home/joachim/.platformio/penv/bin/pio`.
- Local config lives in `config.ini` (gitignored) based on `config.ini.template`. `pre_build.py` may create it and generates `data/build_info.json` for LittleFS build diagnostics.
- When changing files under `data/` (Modbus JSON, build_info, etc), run `/home/joachim/.platformio/penv/bin/pio run -t uploadfs` so the filesystem matches firmware; see `WEB_API.md` `/api/buildinfo`.
- uploads sometimes fail after upload started transferring data; retrying usually works.
- use the configured default hostname, user and password to access the device over web api.
- OTA updates require both firmware and filesystem images to be in sync; if you only upload firmware, you may see a `firmwareFilesystemMismatch` warning in `/api/buildinfo`.
- Your terminal sometimes crashes while you wait for results; just re-run the command.

- Firmware is modular “Feature” architecture (`src/Feature.h`): `setup()` and `loop()` must be non-blocking (state-machine + `millis()` timers; no long `delay()` / busy waits).
- Prefer bounded memory and avoid heap churn in hot paths: minimize `new`/`delete` and large temporary `String`s in `loop()`. Also avoid large dynamic JSON objects in hot paths; use streaming parsing when possible.
- MQTT callbacks should avoid per-message heap allocs (e.g., don’t `new[]` for every payload). Prefer a reusable/static buffer and remember PubSubClient payload isn’t null-terminated.
- For large web responses, prefer streaming (`AsyncResponseStream`) over building giant `String`s.
- Use `LOG_*` macros for observability; keep log volume reasonable on serial.
- Serial log cannot be accessed, but `LOG_*` macros also write to syslog on host `job4`: `ssh job4 sudo tail -f /var/log/messages | grep -v influxd | grep Joba_Esp32` (requires ssh agent).

- Web is `ESPAsyncWebServer` (`WebServerFeature`). Basic auth is enabled only if username+password are configured; `/health` is intended to be unauthenticated.
- Modbus definitions are JSON files in LittleFS (see `data/modbus/`). `MODBUS_LISTEN_ONLY` must prevent any bus transmissions (writes/raw reads) while still allowing monitoring/sniffing.
- MQTT uses `PubSubClient` with a 1024-byte buffer; base topic publishes retained `<base>/status = online`. Home Assistant discovery is under `homeassistant/...` and uses that availability topic.

- suggest commits before adding new unrelated code/features
- write unit tests for new features and bug fixes

---
applyTo: "docs/**/*.md"
---
# Docs conventions

- Keep `README.md`, `SPEC.md`, and `WEB_API.md` aligned with behavior. If you add/rename endpoints or change auth/parameters, update `WEB_API.md`.
- Prefer short, actionable steps (commands, expected outputs, common failure modes like OTA connectivity or missing `uploadfs`).

