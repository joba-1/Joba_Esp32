#include "WebServerFeature.h"
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "StorageFeature.h"
#include <ArduinoJson.h>
#include "TimeUtils.h"
#include "ResetManager.h"
#include <WiFi.h>

#include <esp_ota_ops.h>

#include "ResetDiagnostics.h"

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

void WebServerFeature::setupDefaultRoutes() {
    // Root endpoint - basic info
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }
        
        String title = String(DeviceInfo::getFirmwareName()) + " " + DeviceInfo::getDeviceId();
        String html = "<!DOCTYPE html><html><head><title>" + title + "</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>"
            "body{font-family:Arial,sans-serif;margin:20px;}"
            "h2{margin-top:22px;}"
            ".card{border:1px solid #ddd;border-radius:8px;padding:12px;margin:10px 0;}"
            "code{background:#f6f6f6;padding:1px 4px;border-radius:4px;}"
            "form{margin:8px 0;padding:8px;background:#fafafa;border:1px solid #eee;border-radius:6px;}"
            "label{display:inline-block;margin-right:10px;margin-bottom:6px;}"
            "input,select{padding:4px 6px;}"
            "button{padding:5px 10px;}"
            "small{color:#666;}"
            "</style></head>";
        html += "<body><h1>" + title + "</h1>";
        html += "<p>IP: " + WiFi.localIP().toString() + "</p>";
        html += "<p>Uptime: " + String(millis() / 1000) + " seconds</p>";
        html += "<p>Free Heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
        html += "<div class='card'>";
        html += "<h2>System</h2>";
        html += "<p><a href='/health?json'>/health?json</a> <small>(health check, no auth)</small></p>";
        html += "<p><a href='/api/status'>/api/status</a></p>";
        html += "<p><a href='/api/buildinfo'>/api/buildinfo</a></p>";
        html += "<form action='/api/reset' method='post' onsubmit=\"return confirm('Restart device now?')\">"
            "<strong>/api/reset</strong> <small>(POST)</small> "
            "<label>delayMs <input name='delayMs' type='number' value='250' min='50' max='10000'></label>"
            "<button type='submit'>Restart</button>"
            "</form>";
        html += "</div>";

        html += "<div class='card'>";
        html += "<h2>Storage</h2>";
        html += "<p><a href='/api/storage'>/api/storage</a></p>";
        html += "<form action='/api/storage/list' method='get'>"
            "<strong>/api/storage/list</strong> "
            "<label>path <input name='path' type='text' value='/' size='30'></label>"
            "<button type='submit'>GET</button>"
            "</form>";
        html += "<form action='/api/storage/file' method='get'>"
            "<strong>/api/storage/file</strong> "
            "<label>path <input name='path' type='text' value='/data/sensors.json' size='30'></label>"
            "<button type='submit'>GET</button>"
            "</form>";
        html += "<p><a href='/view/storage'>/view/storage</a> <small>(HTML file browser)</small></p>";
        html += "</div>";

        html += "<div class='card'>";
        html += "<h2>Data Collection</h2>";
        html += "<p><a href='/api/sensors'>/api/sensors</a></p>";
        html += "<p><a href='/api/sensors/latest'>/api/sensors/latest</a></p>";
        html += "<p><a href='/view/sensors'>/view/sensors</a> <small>(HTML table)</small></p>";
        html += "</div>";

        html += "<div class='card'>";
        html += "<h2>Modbus</h2>";
        html += "<p><a href='/api/modbus/status'>/api/modbus/status</a></p>";
        html += "<p><a href='/api/modbus/devices'>/api/modbus/devices</a></p>";
        html += "<p><a href='/api/modbus/maps'>/api/modbus/maps</a></p>";
        html += "<p><a href='/api/modbus/types'>/api/modbus/types</a></p>";
        html += "<p><a href='/api/modbus/monitor'>/api/modbus/monitor</a></p>";
        html += "<p><a href='/view/modbus'>/view/modbus</a> <small>(HTML dashboard)</small></p>";
        html += "<p><a href='/view/modbus/raw'>/view/modbus/raw</a> <small>(raw request tool)</small></p>";

        html += "<form action='/api/modbus/device' method='get'>"
            "<strong>/api/modbus/device</strong> "
            "<label>unit <input name='unit' type='number' value='1' min='1' max='247'></label>"
            "<label><input name='meta' type='checkbox' value='1'> meta</label>"
            "<button type='submit'>GET</button>"
            "</form>";

        html += "<form action='/api/modbus/read' method='get'>"
            "<strong>/api/modbus/read</strong> "
            "<label>unit <input name='unit' type='number' value='1' min='1' max='247'></label>"
            "<label>register <input name='register' type='text' value='' placeholder='e.g. grid_voltage' size='20'></label>"
            "<button type='submit'>GET</button>"
            "</form>";

        html += "<form action='/api/modbus/raw/read' method='get'>"
            "<strong>/api/modbus/raw/read</strong> "
            "<label>unit <input name='unit' type='number' value='1' min='1' max='247'></label>"
            "<label>address <input name='address' type='number' value='0' min='0' max='65535'></label>"
            "<label>count <input name='count' type='number' value='2' min='1' max='125'></label>"
            "<label>fc <select name='fc'><option value='3'>3</option><option value='4'>4</option></select></label>"
            "<button type='submit'>GET</button>"
            "</form>";

        html += "<form action='/api/modbus/write' method='post'>"
            "<strong>/api/modbus/write</strong> <small>(POST)</small> "
            "<label>unit <input name='unit' type='number' value='1' min='1' max='247'></label>"
            "<label>register <input name='register' type='text' value='' placeholder='e.g. inverter_enable' size='20'></label>"
            "<label>value <input name='value' type='number' value='0' step='0.01'></label>"
            "<button type='submit'>POST</button>"
            "</form>";

        html += "</div>";
        html += "</body></html>";
        
        request->send(200, "text/html", html);
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

        String json;
        serializeJson(doc, json);
        request->send(scheduled ? 200 : 409, "application/json", json);
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

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
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

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
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

        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Storage - Files</title>
    <style>
        body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial;margin:20px;background:#1a1a2e;color:#eee}
        .container{max-width:1200px;margin:0 auto}
        h1{color:#00d4ff}
        .table-container{overflow-x:auto;background:#16213e;border-radius:12px;padding:15px}
        table{width:100%;border-collapse:collapse;font-size:0.95em}
        th,td{padding:10px;border-bottom:1px solid #2a2a4a}
        th{background:#0f3460;color:#00d4ff;text-align:left}
        a.btn{background:#00d4ff;color:#1a1a2e;padding:6px 10px;border-radius:6px;text-decoration:none;font-weight:600}
        .path{margin-bottom:10px;color:#ccc}
        .controls{margin-bottom:10px}
    </style>
</head>
<body>
    <div class="container">
        <h1>Storage</h1>
        <div class="controls">
            <button class="btn" onclick="goUp()">Up</button>
            <span style="margin-left:10px;color:#ccc">Current: <span id="currentPath">/</span></span>
            <span style="margin-left:20px;font-size:0.9em;color:#999" id="statusMsg"></span>
        </div>
        <div class="table-container">
            <table id="filesTable">
                <thead>
                    <tr><th>Name</th><th>Size</th><th>Type</th><th>Actions</th></tr>
                </thead>
                <tbody id="filesBody"></tbody>
            </table>
            <div id="noData" style="display:none;padding:20px;color:#666">No files</div>
        </div>
    </div>

    <script>
        const LIST_API = '/api/storage/list';
        const FILE_API = '/api/storage/file';
        let currentPath = '/';

        function humanSize(bytes) {
            if (bytes === undefined || bytes === null) return '-';
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + ' KB';
            return (bytes/(1024*1024)).toFixed(2) + ' MB';
        }

        async function loadPath(path) {
            try {
                const resp = await fetch(LIST_API + '?path=' + encodeURIComponent(path));
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                const data = await resp.json();
                document.getElementById('currentPath').textContent = path;
                document.getElementById('statusMsg').textContent = '';
                currentPath = path;
                const tbody = document.getElementById('filesBody');
                
                if (!data || data.length === 0) {
                    tbody.innerHTML = '';
                    document.getElementById('noData').style.display = 'block';
                    document.getElementById('statusMsg').textContent = 'Empty directory';
                    return;
                }
                document.getElementById('noData').style.display = 'none';
                tbody.innerHTML = data.map(item => {
                    const name = item.name;
                    const isDir = item.isDir;
                    const size = item.size;
                    const displayName = name.replace(/^\//, '');
                    const action = isDir ? `<button class="btn" onclick="loadPath('${name}')">Open</button>` : `<a class="btn" href="${FILE_API}?path=${encodeURIComponent(name)}">Download</a>`;
                    return `<tr><td>${displayName}</td><td>${isDir ? '-' : humanSize(size)}</td><td>${isDir ? 'dir' : 'file'}</td><td>${action}</td></tr>`;
                }).join('');
                document.getElementById('statusMsg').textContent = 'Loaded ' + data.length + ' entries';
            } catch (e) {
                document.getElementById('statusMsg').textContent = 'Error: ' + e.message;
            }
        }

        function goUp() {
            if (currentPath === '/') return;
            let p = currentPath.replace(/\/+$/, '');
            if (p === '') p = '/';
            const idx = p.lastIndexOf('/');
            const parent = idx <= 0 ? '/' : p.substring(0, idx);
            loadPath(parent);
        }

        loadPath('/');
    </script>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", html);
    });
    
    // Health check endpoint (no auth required)
    _server->on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (request->hasParam("json")) {
            ResetDiagnostics::init();

            JsonDocument doc;
            doc["status"] = "ok";

            JsonObject updated = doc["updated"].to<JsonObject>();
            updated["uptimeMs"] = (uint32_t)millis();

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

            String out;
            serializeJson(doc, out);
            request->send(200, "application/json", out);
            return;
        }

        request->send(200, "text/plain", "OK");
    });
    
    // 404 handler
    _server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "Not Found");
    });
}

AsyncWebServer* WebServerFeature::getServer() {
    return _server;
}

void WebServerFeature::addHandler(AsyncWebHandler* handler) {
    if (_server) {
        _server->addHandler(handler);
    }
}

void WebServerFeature::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest) {
    if (_server) {
        _server->on(uri, method, onRequest);
    }
}

bool WebServerFeature::authenticate(AsyncWebServerRequest* request) {
    if (!_authEnabled) {
        return true;
    }
    return request->authenticate(_username, _password);
}
