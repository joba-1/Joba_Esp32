#include "WebServerFeature.h"
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "StorageFeature.h"
#include <ArduinoJson.h>
#include "TimeUtils.h"
#include "ResetManager.h"
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <esp_ota_ops.h>
#include <Update.h>

#include "ResetDiagnostics.h"
#include "CpuMonitor.h"
#include "WebServerFeature_helper.h"

/**
 * @file WebServerFeature.cpp
 * @brief Async web server feature and HTTP API handlers.
 *
 * Implements the `WebServerFeature` which owns an `AsyncWebServer` and
 * registers the firmware's HTTP endpoints (status, storage, modbus views,
 * OTA, health). Authentication is handled via basic auth when configured.
 */

#ifndef FIRMWARE_GIT_SHA
#define FIRMWARE_GIT_SHA unknown
#endif

#ifndef FIRMWARE_BUILD_UNIX
#define FIRMWARE_BUILD_UNIX 0
#endif

#define _STR_HELPER(x) #x
#define _STR(x) _STR_HELPER(x)
// Access global storage instance defined in main.cpp
extern StorageFeature storage;

/**
 * @brief Initialize the async web server feature.
 *
 * This constructor stores configuration (port, optional basic-auth credentials)
 * but does not start network or server resources. Call `setup()` to allocate
 * and begin the `AsyncWebServer` instance so routes become active.
 */
WebServerFeature::WebServerFeature(uint16_t port, const char* username, const char* password)
    : _port(port)
    , _username(username)
    , _password(password)
    , _authEnabled(strlen(username) > 0 && strlen(password) > 0)
    , _ready(false)
    , _setupDone(false)
    , _server(nullptr)
{
}

/**
 * @brief Create and start the `AsyncWebServer` instance.
 *
 * Idempotent: calling `setup()` multiple times has no effect after the first
 * successful initialization. This performs route registration via
 * `setupDefaultRoutes()` and begins listening on the configured port.
 */
void WebServerFeature::setup() {
    if (_setupDone) return;
    
    LOG_I("Starting async web server on port %d", _port);
    
    // Create server instance
    _server = new AsyncWebServer(_port);
    
    // Setup default routes
    setupDefaultRoutes();
    
    // Start server
    _server->begin();
    
    _ready = true;
    _setupDone = true;
    
    LOG_I("Web server started%s", _authEnabled ? " (auth enabled)" : "");
}

/**
 * @brief Register the default HTTP routes used by the firmware.
 *
 * Routes include static assets, root page, storage API, health checks and
 * the HTTP OTA update endpoint. Authentication checks are performed where
 * required via `authenticate()`.
 */
void WebServerFeature::setupDefaultRoutes() {
    // Serve static CSS file (no auth required)
    _server->on("/style.css", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/style.css", "text/css");
    });

    // Root endpoint - basic info
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        // Generate HTML using helper function
        String deviceId = DeviceInfo::getDeviceId();
        String firmwareName = String(DeviceInfo::getFirmwareName());
        IPAddress ipAddress = WiFi.localIP();
        uint32_t uptimeSeconds = millis() / 1000;
        uint32_t freeHeap = ESP.getFreeHeap();

        WebServerHelper::sendRootPage(request, deviceId, firmwareName, ipAddress, uptimeSeconds, freeHeap);
    });

    // Restart endpoint
    _server->on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        uint32_t delayMs = 250;
        if (request->hasParam("delayMs", true)) {
            delayMs = (uint32_t)request->getParam("delayMs", true)->value().toInt();
        } else if (request->hasParam("delayMs")) {
            delayMs = (uint32_t)request->getParam("delayMs")->value().toInt();
        }

        JsonDocument doc;
        const bool scheduled = ResetManager::scheduleRestart(delayMs, "web");
        doc["scheduled"] = scheduled;
        doc["delayMs"] = (uint32_t)delayMs;
        if (!scheduled) {
            doc["error"] = "Restart already scheduled";
        }

        {
            AsyncResponseStream* response = request->beginResponseStream("application/json");
            serializeJson(doc, *response);
            request->send(response);
        }
    });
    
    // API status endpoint
    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        JsonDocument doc;
        doc["freeHeap"] = (uint32_t)ESP.getFreeHeap();
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = (int32_t)WiFi.RSSI();

        JsonObject updated = doc["updated"].to<JsonObject>();
        updated["uptimeMs"] = (uint32_t)millis();
        const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
        if (nowUnix != 0) {
            updated["epoch"] = nowUnix;
            String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
            if (iso.length() > 0) updated["iso"] = iso;
        }

        {
            AsyncResponseStream* response = request->beginResponseStream("application/json");
            serializeJson(doc, *response);
            request->send(response);
        }
    });

    // Firmware + filesystem build info (requires auth if enabled)
    _server->on("/api/buildinfo", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        JsonDocument doc;
        doc["deviceId"] = DeviceInfo::getDeviceId();
        doc["firmwareName"] = DeviceInfo::getFirmwareName();

        JsonObject updated = doc["updated"].to<JsonObject>();
        updated["uptimeMs"] = (uint32_t)millis();
        const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
        if (nowUnix != 0) {
            updated["epoch"] = nowUnix;
            String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
            if (iso.length() > 0) updated["iso"] = iso;
        }

        JsonObject fw = doc["firmware"].to<JsonObject>();
        fw["gitSha"] = _STR(FIRMWARE_GIT_SHA);

        fw["sketchMd5"] = ESP.getSketchMD5();
        fw["sketchSize"] = (uint32_t)ESP.getSketchSize();
        fw["freeSketchSpace"] = (uint32_t)ESP.getFreeSketchSpace();

        // OTA partition diagnostics (helps debug update/rollback behavior)
        {
            const esp_partition_t* running = esp_ota_get_running_partition();
            const esp_partition_t* boot = esp_ota_get_boot_partition();
            JsonObject ota = fw["ota"].to<JsonObject>();

            if (running) {
                JsonObject r = ota["running"].to<JsonObject>();
                r["label"] = running->label;
                r["address"] = (uint32_t)running->address;
                r["size"] = (uint32_t)running->size;
                r["subtype"] = (uint32_t)running->subtype;
            }
            if (boot) {
                JsonObject b = ota["boot"].to<JsonObject>();
                b["label"] = boot->label;
                b["address"] = (uint32_t)boot->address;
                b["size"] = (uint32_t)boot->size;
                b["subtype"] = (uint32_t)boot->subtype;
            }
        }

        if ((uint32_t)FIRMWARE_BUILD_UNIX != 0) {
            JsonObject built = fw["built"].to<JsonObject>();
            built["epoch"] = (uint32_t)FIRMWARE_BUILD_UNIX;
            String iso = TimeUtils::isoUtcFromUnixSeconds((uint32_t)FIRMWARE_BUILD_UNIX);
            if (iso.length() > 0) built["iso"] = iso;
        }

        JsonObject fs = doc["filesystem"].to<JsonObject>();
        fs["mounted"] = storage.isReady();
        fs["manifestPath"] = "/build_info.json";
        if (storage.isReady() && storage.exists("/build_info.json")) {
            String content = storage.readFile("/build_info.json");
            fs["manifestRawBytes"] = content.length();

            JsonDocument fsDoc;
            DeserializationError err = deserializeJson(fsDoc, content);
            if (!err) {
                fs["manifest"].set(fsDoc.as<JsonVariantConst>());
            } else {
                fs["manifestParseError"] = err.c_str();
            }
        } else if (storage.isReady()) {
            fs["manifestError"] = "build_info.json not found";
        } else {
            fs["manifestError"] = "storage not mounted";
        }

        // Report a mismatch hint if we have both identifiers available
        JsonVariantConst fsManifest = fs["manifest"];
        if (!fsManifest.isNull()) {
            if (fsManifest.is<JsonObjectConst>() && fsManifest["gitCommit"].is<const char*>()) {
                const char* fsCommit = fsManifest["gitCommit"].as<const char*>();
                const char* fwCommit = fw["gitSha"].as<const char*>();
                if (fsCommit && fwCommit && strlen(fsCommit) > 0 && strlen(fwCommit) > 0) {
                    doc["firmwareFilesystemMismatch"] = (strcmp(fsCommit, fwCommit) != 0);
                }
            }
        }

        AsyncResponseStream* response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });

    // IMPORTANT: Register specific storage endpoints BEFORE the general /api/storage endpoint
    // This ensures /api/storage/list and /api/storage/file are matched before /api/storage
    
    // Storage list endpoint, accepts query param 'path' (requires auth)
    _server->on("/api/storage/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        String path = "/";
        if (request->hasParam("path")) {
            path = request->getParam("path")->value();
        }

        if (!storage.isReady()) {
            return request->send(500, "application/json", "{\"error\":\"storage not mounted\"}");
        }

        String list = storage.listDir(path.c_str());
        request->send(200, "application/json", list);
    });

    // File download endpoint - returns file content with attachment header
    _server->on("/api/storage/file", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        if (!storage.isReady()) {
            return request->send(500, "application/json", "{\"error\":\"storage not mounted\"}");
        }

        if (!request->hasParam("path")) {
            return request->send(400, "application/json", "{\"error\":\"missing 'path' parameter\"}");
        }

        String path = request->getParam("path")->value();
        if (!storage.exists(path.c_str())) {
            return request->send(404, "application/json", "{\"error\":\"not found\"}");
        }

        String content = storage.readFile(path.c_str());
        AsyncWebServerResponse* response = request->beginResponse(200, "application/octet-stream", content);
        // Add Content-Disposition header for attachment with filename
        int slash = path.lastIndexOf('/');
        String fname = (slash >= 0) ? path.substring(slash + 1) : path;
        response->addHeader("Content-Disposition", String("attachment; filename=\"") + fname + "\"");
        request->send(response);
    });

    // Storage diagnostics endpoint (requires auth) - REGISTERED LAST so specific routes match first
    _server->on("/api/storage", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        if (!storage.isReady()) {
            return request->send(500, "application/json", "{\"error\":\"storage not mounted\"}");
        }

        String json = "{";
        json += "\"mounted\":true,";
        json += "\"total\":" + String(storage.totalBytes()) + ",";
        json += "\"used\":" + String(storage.usedBytes()) + ",";
        json += "\"free\":" + String(storage.freeBytes()) + ",";
        json += "\"root\":" + storage.listDir("/") + ",";
        json += "\"modbus\":" + storage.listDir("/modbus") + ",";
        json += "\"data\":" + storage.listDir("/data");
        json += "}";

        request->send(200, "application/json", json);
    });

    // Storage HTML view (requires auth if enabled)
    _server->on("/view/storage", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        WebServerHelper::sendStoragePage(request);
    });
    
    // Health check endpoint (no auth required)
    _server->on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (request->hasParam("json")) {
            ResetDiagnostics::init();

            JsonDocument doc;
            doc["status"] = "ok";

            JsonObject updated = doc["updated"].to<JsonObject>();
            updated["uptimeMs"] = (uint32_t)millis();

            JsonObject cpu = doc["cpu"].to<JsonObject>();
            cpu["usagePercent"] = CpuMonitor::usagePercent();
            cpu["busyTimeUs"] = CpuMonitor::busyTimeUs();
            cpu["idleTimeUs"] = CpuMonitor::idleTimeUs();
            cpu["loopCount"] = CpuMonitor::loopCount();
            cpu["avgLoopDurationUs"] = CpuMonitor::avgLoopDurationUs();

            JsonObject reset = doc["reset"].to<JsonObject>();
            reset["bootCount"] = (uint32_t)ResetDiagnostics::bootCount();
            reset["reason"] = ResetDiagnostics::resetReasonString();
            reset["reasonCode"] = (int32_t)ResetDiagnostics::resetReason();
            reset["rtcCore0"] = (uint32_t)ResetDiagnostics::rtcResetReasonCore0();
            reset["rtcCore1"] = (uint32_t)ResetDiagnostics::rtcResetReasonCore1();

            JsonObject breadcrumb = reset["breadcrumb"].to<JsonObject>();
            breadcrumb["phase"] = ResetDiagnostics::breadcrumbPhase();
            breadcrumb["name"] = ResetDiagnostics::breadcrumbName();
            breadcrumb["uptimeMs"] = (uint32_t)ResetDiagnostics::breadcrumbUptimeMs();

            JsonObject lastLoop = reset["lastLoop"].to<JsonObject>();
            lastLoop["name"] = ResetDiagnostics::lastLoopName();
            lastLoop["durationUs"] = (uint32_t)ResetDiagnostics::lastLoopDurationUs();

            JsonObject maxLoop = reset["maxLoop"].to<JsonObject>();
            maxLoop["name"] = ResetDiagnostics::maxLoopName();
            maxLoop["durationUs"] = (uint32_t)ResetDiagnostics::maxLoopDurationUs();

            doc["freeHeap"] = (uint32_t)ESP.getFreeHeap();

            {
                AsyncResponseStream* response = request->beginResponseStream("application/json");
                serializeJson(doc, *response);
                request->send(response);
            }
            return;
        }

        request->send(200, "text/plain", "OK");
    });

    // HTTP OTA firmware update endpoint
    // Usage: curl -u admin:password -F "firmware=@.pio/build/serial/firmware.bin" http://device/api/update
    _server->on("/api/update", HTTP_POST, 
        // Request handler (called after upload completes)
        [this](AsyncWebServerRequest* request) {
            Serial.println("[OTA] Request handler called");
            if (_authEnabled && !authenticate(request)) {
                Serial.println("[OTA] Auth failed in request handler");
                return request->requestAuthentication();
            }
            
            bool success = !Update.hasError();
            AsyncWebServerResponse* response = request->beginResponse(
                success ? 200 : 500, 
                "application/json",
                success ? "{\"status\":\"ok\",\"message\":\"Update successful, rebooting...\"}" 
                        : "{\"status\":\"error\",\"message\":\"Update failed\"}"
            );
            response->addHeader("Connection", "close");
            request->send(response);
            
            if (success) {
                LOG_I("HTTP OTA update successful, scheduling restart");
                ResetManager::scheduleRestart(1000, "http_ota");
            }
        },
        // File upload handler (multipart)
        [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (_authEnabled && !authenticate(request)) {
                return;
            }
            
            if (index == 0) {
                Serial.printf("\n[OTA] Starting: %s, free heap: %u\n", filename.c_str(), ESP.getFreeHeap());
                LOG_I("HTTP OTA update starting: %s", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Serial.printf("[OTA] Begin FAILED: %s\n", Update.errorString());
                    LOG_E("HTTP OTA begin failed: %s", Update.errorString());
                    return;
                }
                Serial.println("[OTA] Begin OK");
            }
            
            if (len > 0) {
                size_t written = Update.write(data, len);
                if (written != len) {
                    Serial.printf("[OTA] Write FAILED at %u: %s\n", index, Update.errorString());
                    LOG_E("HTTP OTA write failed: %s", Update.errorString());
                    return;
                }
                if ((index % 102400) < len) {
                    Serial.printf("[OTA] %uKB, heap: %u\n", (index + len) / 1024, ESP.getFreeHeap());
                }
                esp_task_wdt_reset();
                yield();
            }
            
            if (final) {
                Serial.printf("[OTA] Finalizing at %u bytes\n", index + len);
                if (Update.end(true)) {
                    Serial.println("[OTA] SUCCESS");
                    LOG_I("HTTP OTA update complete: %u bytes", index + len);
                } else {
                    Serial.printf("[OTA] End FAILED: %s\n", Update.errorString());
                    LOG_E("HTTP OTA end failed: %s", Update.errorString());
                }
            }
        }
    );
    
    // 404 handler
    _server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "Not Found");
    });
}

/**
 * @brief Access the underlying `AsyncWebServer` instance.
 *
 * Returns nullptr if `setup()` has not been called yet. Callers should not
 * take ownership of the returned pointer.
 */
AsyncWebServer* WebServerFeature::getServer() {
    return _server;
}

/**
 * @brief Add a custom `AsyncWebHandler` to the server.
 *
 * Forwards to the internal server when available. The server retains
 * ownership of the handler.
 */
void WebServerFeature::addHandler(AsyncWebHandler* handler) {
    if (_server) {
        _server->addHandler(handler);
    }
}

/**
 * @brief Convenience wrapper to register a URI handler with the server.
 */
void WebServerFeature::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest) {
    if (_server) {
        _server->on(uri, method, onRequest);
    }
}

/**
 * @brief Authenticate a request when basic auth is enabled.
 *
 * Returns true when authentication is disabled or the provided credentials
 * are valid.
 */
bool WebServerFeature::authenticate(AsyncWebServerRequest* request) {
    if (!_authEnabled) {
        return true;
    }
    return request->authenticate(_username, _password);
}
