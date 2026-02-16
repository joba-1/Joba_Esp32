#include "WebServerFeature_helper.h"
#include <ESPAsyncWebServer.h>

namespace WebServerHelper {

void sendStoragePage(AsyncWebServerRequest* request) {
    AsyncResponseStream *response = request->beginResponseStream(F("text/html"));
    response->print(F(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Storage - Files</title>
    <link rel="stylesheet" href="/style.css">
    <style>
        h1, a, .btn, button, th {color:#7E57C2!important}
        .btn, button {background:#7E57C2!important}
        .btn:hover, button:hover {background:#6A1B9A!important}
    </style>
</head>
<body>
    <div class="container">
        <a href="/" class="home-link">Home</a>
        <h1>Storage Browser</h1>
        <div class="controls">
            <button class="btn" onclick="goUp()">Up</button>
            <span style="margin-left:10px;color:#ccc">Current: <span id="currentPath">/</span></span>
            <span style="margin-left:20px;font-size:0.9em;color:#888" id="statusMsg"></span>
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
            let p = currentPath.replace(/\/+$, '');
            if (p === '') p = '/';
            const idx = p.lastIndexOf('/');
            const parent = idx <= 0 ? '/' : p.substring(0, idx);
            loadPath(parent);
        }

        loadPath('/');
    </script>
</body>
</html>
)rawliteral"));

    request->send(response);
}

void sendRootPage(AsyncWebServerRequest* request, const String& deviceId, const String& firmwareName,
                  const IPAddress& ipAddress, uint32_t uptimeSeconds, uint32_t freeHeap) {
    String title;
    if (deviceId.startsWith(firmwareName)) {
        title = deviceId;
    } else {
        title = firmwareName + " " + deviceId;
    }

    AsyncResponseStream *response = request->beginResponseStream(F("text/html"));

    // Head and title (static in flash)
    response->print(F("<!DOCTYPE html><html><head><title>"));
    response->print(title);
    response->print(F("</title>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<link rel='stylesheet' href='/style.css'>"
        "</head>"
        "<body><div class='container'>"
        "<h1>"));

    response->print(title);

    // Dynamic status line up to freeHeap
    response->print(F("</h1><p style='color:#888'>IP: "));
    response->print(ipAddress.toString());
    response->print(F(" | Uptime: "));
    response->print(String(uptimeSeconds));
    response->print(F("s | Heap: "));
    response->print(String(freeHeap));

    // Print the large remainder from flash
    response->print(F(" bytes</p>"
        "<div class='card'>"
        "<h2>System</h2>"
        "<p><a href='/health?json'>Health Check</a> <small>(no auth, JSON)</small></p>"
        "<p><a href='/api/status'>Device Status</a> <small>(JSON)</small></p>"
        "<p><a href='/api/buildinfo'>Build Information</a> <small>(JSON)</small></p>"
        "<form action='/api/reset' method='post' onsubmit=\"return confirm('Restart device now?')\">"
        "<strong>/api/reset</strong> <small>(POST)</small> "
        "<label>delayMs <input name='delayMs' type='number' value='250' min='50' max='10000'></label>"
        "<button type='submit'>Restart</button>"
        "</form>"
        "<form action='/api/update' method='post' enctype='multipart/form-data' onsubmit=\"return confirm('Upload firmware and reboot?')\">"
        "<strong>/api/update</strong> <small>(HTTP OTA)</small> "
        "<input type='file' name='firmware' accept='.bin'> "
        "<button type='submit'>Upload</button>"
        "</form>"
        "</div>"

        "<div class='card'>"
        "<h2>Modbus</h2>"
        "<p><a href='/view/modbus'>Modbus Dashboard</a> <small>(live dashboard)</small></p>"
        "<p><a href='/view/modbus/decoded'>Decoded Register Viewer</a> <small>(interactive)</small></p>"
        "<p><a href='/view/modbus/raw'>Raw Request Tool</a> <small>(low-level debugging)</small></p>"
        "<p><a href='/view/modbus/patterns'>Bus Pattern Analysis</a> <small>(traffic analysis)</small></p>"
        "<p><a href='/view/modbus/scheduler'>Gap Scheduler Monitor</a> <small>(TX scheduling)</small></p>"
        "<p><a href='/view/modbus/diagnostics'>Modbus Diagnostics</a> <small>(JSON & tools)</small></p>"

        "<div class='card'>"
        "<h2>Storage</h2>"
        "<p><a href='/api/storage'>Storage Status</a> <small>(JSON)</small></p>"
        "<p><a href='/view/storage'>File Browser</a> <small>(interactive)</small></p>"
        "<form action='/api/storage/list' method='get'>"
        "<strong>/api/storage/list</strong> "
        "<label>path <input name='path' type='text' value='/' size='30'></label>"
        "<button type='submit'>GET</button>"
        "</form>"
        "<form action='/api/storage/file' method='get'>"
        "<strong>/api/storage/file</strong> "
        "<label>path <input name='path' type='text' value='/data/sensors.json' size='30'></label>"
        "<button type='submit'>GET</button>"
        "</form>"
        "</div>"

        "<div class='card'>"
        "<h2>Data Collection</h2>"
        "<p><a href='/view/sensors'>Sensors Dashboard</a> <small>(live table)</small></p>"
        "<p><a href='/api/sensors'>All Sensor Data</a> <small>(JSON)</small></p>"
        "<p><a href='/api/sensors/latest'>Latest Sensor Values</a> <small>(JSON)</small></p>"
        "</div>"

        "</div></body></html>"));

    request->send(response);
}

} // namespace WebServerHelper
