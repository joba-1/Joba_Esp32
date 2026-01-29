#include "WebServerFeature.h"
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include "StorageFeature.h"
#include <WiFi.h>

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
        html += "<style>body{font-family:Arial,sans-serif;margin:20px;}</style></head>";
        html += "<body><h1>" + title + "</h1>";
        html += "<p>IP: " + WiFi.localIP().toString() + "</p>";
        html += "<p>Uptime: " + String(millis() / 1000) + " seconds</p>";
        html += "<p>Free Heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
        html += "<p><a href='/api/status'>API Status</a></p>";
        html += "</body></html>";
        
        request->send(200, "text/html", html);
    });
    
    // API status endpoint
    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }
        
        String json = "{";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI());
        json += "}";
        
        request->send(200, "application/json", json);
    });

    // Storage diagnostics endpoint (requires auth)
    _server->on("/api/storage", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_authEnabled && !authenticate(request)) {
            return request->requestAuthentication();
        }

        if (!storage.isReady()) {
            return request->send(500, "application/json", "{\"error\":\"storage not mounted\"}");
        }

        // Debug: scan entire filesystem to see what's actually there (with timeout)
        JsonDocument debugDoc;
        JsonArray allFiles = debugDoc["allFiles"].to<JsonArray>();
        
        File root = LittleFS.open("/");
        if (root && root.isDirectory()) {
            File file = root.openNextFile();
            unsigned long startTime = millis();
            const unsigned long TIMEOUT = 100;  // 100ms max
            unsigned int count = 0;
            while (file && (millis() - startTime) < TIMEOUT && count < 100) {
                String fname = String(file.name());
                JsonObject fobj = allFiles.add<JsonObject>();
                fobj["name"] = fname;
                fobj["size"] = file.size();
                fobj["isDir"] = file.isDirectory();
                file = root.openNextFile();
                count++;
            }
            if ((millis() - startTime) >= TIMEOUT) {
                debugDoc["debug_timeout"] = true;
            }
        }

        String json = "{";
        json += "\"mounted\":true,";
        json += "\"total\":" + String(storage.totalBytes()) + ",";
        json += "\"used\":" + String(storage.usedBytes()) + ",";
        json += "\"free\":" + String(storage.freeBytes()) + ",";
        json += "\"root\":" + storage.listDir("/") + ",";
        json += "\"modbus\":" + storage.listDir("/modbus") + ",";
        json += "\"data\":" + storage.listDir("/data");
        
        // Add debug info
        String debugStr;
        serializeJson(debugDoc, debugStr);
        json += ",\"debug\":" + debugStr;
        
        json += "}";

        request->send(200, "application/json", json);
    });

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
                console.log('Loading path:', path);
                const resp = await fetch(LIST_API + '?path=' + encodeURIComponent(path));
                console.log('Response status:', resp.status);
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                const rawText = await resp.text();
                console.log('Raw response:', rawText);
                const data = JSON.parse(rawText);
                console.log('Parsed data:', data, 'is array:', Array.isArray(data));
                document.getElementById('currentPath').textContent = path;
                document.getElementById('statusMsg').textContent = '';
                currentPath = path;
                const tbody = document.getElementById('filesBody');
                
                // Handle both array and object responses
                const items = Array.isArray(data) ? data : (data.entries || data.items || []);
                
                if (!items || items.length === 0) {
                    tbody.innerHTML = '';
                    document.getElementById('noData').style.display = 'block';
                    document.getElementById('statusMsg').textContent = 'Empty directory';
                    return;
                }
                document.getElementById('noData').style.display = 'none';
                tbody.innerHTML = items.map(item => {
                    const name = item.name;
                    const isDir = item.isDir;
                    const size = item.size;
                    const displayName = name.replace(/^\//, '');
                    const rel = name;
                    const action = isDir ? `<button class="btn" onclick="loadPath('${rel}')">Open</button>` : `<a class="btn" href="${FILE_API}?path=${encodeURIComponent(rel)}">Download</a>`;
                    return `<tr><td>${displayName}</td><td>${isDir ? '-' : humanSize(size)}</td><td>${isDir ? 'dir' : 'file'}</td><td>${action}</td></tr>`;
                }).join('');
                document.getElementById('statusMsg').textContent = 'Loaded ' + items.length + ' entries';
            } catch (e) {
                console.error('Load error', e);
                document.getElementById('statusMsg').textContent = 'Error: ' + e.message;
            }
        }

        function goUp() {
            if (currentPath === '/') return;
            let p = currentPath.replace(/\/+$/,'');
            if (p === '') p = '/';
            const idx = p.lastIndexOf('/');
            const parent = idx <= 0 ? '/' : p.substring(0, idx);
            loadPath(parent);
        }

        // Initial load
        loadPath('/');
    </script>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", html);
    });
    
    // Health check endpoint (no auth required)
    _server->on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
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
