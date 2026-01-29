#ifndef DATA_COLLECTION_WEB_H
#define DATA_COLLECTION_WEB_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "DataCollection.h"
#include "WebServerFeature.h"

/**
 * @brief Helper to register web endpoints for a DataCollection
 * 
 * Registers two endpoints per collection:
 * - /api/<name>         - JSON API returning all data
 * - /api/<name>/latest  - JSON API returning latest entry
 * - /view/<name>        - HTML table view with auto-refresh
 */
class DataCollectionWeb {
public:
    /**
     * @brief Register web endpoints for a data collection
     * @param server AsyncWebServer instance
     * @param basePath Base path for endpoints (e.g., "sensors" -> /api/sensors, /view/sensors)
     * @param getJsonCallback Callback that returns JSON string of all data
     * @param getLatestJsonCallback Callback that returns JSON string of latest entry
     * @param getSchemaCallback Callback that returns field names as JSON array
     * @param refreshIntervalMs Auto-refresh interval for HTML view (default 5000ms)
     */
    // Register endpoints using a raw AsyncWebServer instance (no auth check)
    static void registerEndpoints(
        AsyncWebServer* server,
        const char* basePath,
        std::function<String()> getJsonCallback,
        std::function<String()> getLatestJsonCallback,
        std::function<String()> getSchemaCallback,
        uint32_t refreshIntervalMs = 5000
    ) {
        String apiPath = String("/api/") + basePath;
        String apiLatestPath = apiPath + "/latest";
        String viewPath = String("/view/") + basePath;
        
        // Store paths in heap for lambda capture
        char* apiPathStr = strdup(apiPath.c_str());
        char* viewPathStr = strdup(viewPath.c_str());
        
        // API endpoint - all data
        server->on(apiPathStr, HTTP_GET, [getJsonCallback](AsyncWebServerRequest* request) {
            request->send(200, "application/json", getJsonCallback());
        });
        
        // API endpoint - latest entry
        server->on(strdup(apiLatestPath.c_str()), HTTP_GET, [getLatestJsonCallback](AsyncWebServerRequest* request) {
            String json = getLatestJsonCallback();
            if (json.length() == 0 || json == "{}") {
                request->send(404, "application/json", "{\"error\":\"No data available\"}");
            } else {
                request->send(200, "application/json", json);
            }
        });
        
        // HTML view endpoint
        server->on(viewPathStr, HTTP_GET, [basePath, apiPathStr, getSchemaCallback, refreshIntervalMs](AsyncWebServerRequest* request) {
            String html = generateHtmlView(basePath, apiPathStr, getSchemaCallback(), refreshIntervalMs);
            request->send(200, "text/html", html);
        });
    }

    // Register endpoints using WebServerFeature (enforces auth if enabled)
    static void registerEndpoints(
        WebServerFeature& serverFeature,
        const char* basePath,
        std::function<String()> getJsonCallback,
        std::function<String()> getLatestJsonCallback,
        std::function<String()> getSchemaCallback,
        uint32_t refreshIntervalMs = 5000
    ) {
        AsyncWebServer* server = serverFeature.getServer();
        String apiPath = String("/api/") + basePath;
        String apiLatestPath = apiPath + "/latest";
        String viewPath = String("/view/") + basePath;
        
        // Store paths in heap for lambda capture
        char* apiPathStr = strdup(apiPath.c_str());
        char* viewPathStr = strdup(viewPath.c_str());
        
        // API endpoint - all data
        server->on(apiPathStr, HTTP_GET, [getJsonCallback, &serverFeature](AsyncWebServerRequest* request) {
            if (!serverFeature.authenticate(request)) return request->requestAuthentication();
            request->send(200, "application/json", getJsonCallback());
        });
        
        // API endpoint - latest entry
        server->on(strdup(apiLatestPath.c_str()), HTTP_GET, [getLatestJsonCallback, &serverFeature](AsyncWebServerRequest* request) {
            if (!serverFeature.authenticate(request)) return request->requestAuthentication();
            String json = getLatestJsonCallback();
            if (json.length() == 0 || json == "{}") {
                request->send(404, "application/json", "{\"error\":\"No data available\"}");
            } else {
                request->send(200, "application/json", json);
            }
        });
        
        // HTML view endpoint
        server->on(viewPathStr, HTTP_GET, [basePath, apiPathStr, getSchemaCallback, refreshIntervalMs, &serverFeature](AsyncWebServerRequest* request) {
            if (!serverFeature.authenticate(request)) return request->requestAuthentication();
            String html = generateHtmlView(basePath, apiPathStr, getSchemaCallback(), refreshIntervalMs);
            request->send(200, "text/html", html);
        });
    }
    
    /**
     * @brief Convenience method to register endpoints for a DataCollection instance
     */
    template<typename T, size_t N>
    static void registerCollection(
        AsyncWebServer* server,
        DataCollection<T, N>& collection,
        const char* basePath,
        uint32_t refreshIntervalMs = 5000
    ) {
        // Create schema JSON from collection
        auto getSchema = [&collection]() -> String {
            return getFieldNames(collection);
        };
        
        registerEndpoints(
            server,
            basePath,
            [&collection]() { return collection.toJson(); },
            [&collection]() { 
                if (collection.isEmpty()) return String("{}");
                return collection.toJson(collection.count() - 1);
            },
            getSchema,
            refreshIntervalMs
        );
    }

    // Overload that accepts WebServerFeature to enforce auth when enabled
    template<typename T, size_t N>
    static void registerCollection(
        WebServerFeature& serverFeature,
        DataCollection<T, N>& collection,
        const char* basePath,
        uint32_t refreshIntervalMs = 5000
    ) {
        // Create schema JSON from collection
        auto getSchema = [&collection]() -> String {
            return getFieldNames(collection);
        };
        
        registerEndpoints(
            serverFeature,
            basePath,
            [&collection]() { return collection.toJson(); },
            [&collection]() { 
                if (collection.isEmpty()) return String("{}");
                return collection.toJson(collection.count() - 1);
            },
            getSchema,
            refreshIntervalMs
        );
    }

private:
    template<typename T, size_t N>
    static String getFieldNames(DataCollection<T, N>& collection) {
        // This would require access to schema, which we don't have here
        // Return empty array - the HTML will handle it
        return "[]";
    }
    
    static String generateHtmlView(const char* name, const char* apiPath, const String& schema, uint32_t refreshIntervalMs) {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)rawliteral";
        html += name;
        html += R"rawliteral( - Data View</title>
    <style>
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: #1a1a2e;
            color: #eee;
            padding: 20px;
            min-height: 100vh;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        h1 {
            color: #00d4ff;
            margin-bottom: 10px;
            font-size: 1.8em;
        }
        .status {
            display: flex;
            gap: 20px;
            margin-bottom: 20px;
            flex-wrap: wrap;
        }
        .status-item {
            background: #16213e;
            padding: 10px 15px;
            border-radius: 8px;
            font-size: 0.9em;
        }
        .status-item span {
            color: #00d4ff;
            font-weight: bold;
        }
        .status-dot {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            margin-right: 8px;
            animation: pulse 2s infinite;
        }
        .status-dot.connected { background: #00ff88; }
        .status-dot.disconnected { background: #ff4444; }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .table-container {
            overflow-x: auto;
            background: #16213e;
            border-radius: 12px;
            padding: 15px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.9em;
        }
        th, td {
            padding: 12px 15px;
            text-align: left;
            border-bottom: 1px solid #2a2a4a;
        }
        th {
            background: #0f3460;
            color: #00d4ff;
            font-weight: 600;
            position: sticky;
            top: 0;
        }
        tr:hover {
            background: #1f3a5f;
        }
        tr:last-child td {
            border-bottom: none;
        }
        .no-data {
            text-align: center;
            padding: 40px;
            color: #666;
        }
        .refresh-info {
            text-align: right;
            font-size: 0.8em;
            color: #666;
            margin-top: 10px;
        }
        .btn {
            background: #00d4ff;
            color: #1a1a2e;
            border: none;
            padding: 8px 16px;
            border-radius: 6px;
            cursor: pointer;
            font-weight: 600;
            margin-left: 10px;
        }
        .btn:hover {
            background: #00a8cc;
        }
        .latest {
            background: #1f4a3f !important;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>)rawliteral";
        html += name;
        html += R"rawliteral(</h1>
        <div class="status">
            <div class="status-item">
                <span class="status-dot connected" id="statusDot"></span>
                <span id="statusText">Connected</span>
            </div>
            <div class="status-item">Entries: <span id="entryCount">0</span></div>
            <div class="status-item">Last Update: <span id="lastUpdate">-</span></div>
            <button class="btn" onclick="fetchData()">Refresh Now</button>
        </div>
        <div class="table-container">
            <table id="dataTable">
                <thead id="tableHead"></thead>
                <tbody id="tableBody"></tbody>
            </table>
            <div class="no-data" id="noData" style="display:none;">No data available</div>
        </div>
        <div class="refresh-info">Auto-refresh every )rawliteral";
        html += String(refreshIntervalMs / 1000);
        html += R"rawliteral( seconds</div>
    </div>

    <script>
        const API_URL = ')rawliteral";
        html += apiPath;
        html += R"rawliteral(';
        const REFRESH_INTERVAL = )rawliteral";
        html += String(refreshIntervalMs);
        html += R"rawliteral(;
        
        let columns = [];
        let lastData = null;
        
        function formatValue(key, value) {
            if (value === null || value === undefined) return '-';
            if (key === 'timestamp' || key.includes('time')) {
                if (typeof value === 'number' && value > 1000000000) {
                    const date = new Date(value * 1000);
                    return date.toLocaleString();
                }
            }
            if (typeof value === 'number') {
                if (Number.isInteger(value)) return value.toString();
                return value.toFixed(2);
            }
            if (typeof value === 'boolean') return value ? 'Yes' : 'No';
            return String(value);
        }
        
        function updateTable(data) {
            const thead = document.getElementById('tableHead');
            const tbody = document.getElementById('tableBody');
            const noData = document.getElementById('noData');
            const entryCount = document.getElementById('entryCount');
            
            if (!data || data.length === 0) {
                thead.innerHTML = '';
                tbody.innerHTML = '';
                noData.style.display = 'block';
                entryCount.textContent = '0';
                return;
            }
            
            noData.style.display = 'none';
            entryCount.textContent = data.length;
            
            // Get columns from first entry
            if (columns.length === 0 && data.length > 0) {
                columns = Object.keys(data[0]);
            }
            
            // Build header
            thead.innerHTML = '<tr>' + columns.map(col => 
                `<th>${col}</th>`
            ).join('') + '</tr>';
            
            // Build rows (newest first)
            const reversedData = [...data].reverse();
            tbody.innerHTML = reversedData.map((row, idx) => 
                `<tr class="${idx === 0 ? 'latest' : ''}">${columns.map(col => 
                    `<td>${formatValue(col, row[col])}</td>`
                ).join('')}</tr>`
            ).join('');
        }
        
        async function fetchData() {
            try {
                const response = await fetch(API_URL);
                if (!response.ok) throw new Error('HTTP ' + response.status);
                
                const data = await response.json();
                lastData = data;
                updateTable(data);
                
                document.getElementById('statusDot').className = 'status-dot connected';
                document.getElementById('statusText').textContent = 'Connected';
                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            } catch (error) {
                console.error('Fetch error:', error);
                document.getElementById('statusDot').className = 'status-dot disconnected';
                document.getElementById('statusText').textContent = 'Disconnected';
            }
        }
        
        // Initial fetch
        fetchData();
        
        // Auto-refresh
        setInterval(fetchData, REFRESH_INTERVAL);
    </script>
</body>
</html>
)rawliteral";
        return html;
    }
};

#endif // DATA_COLLECTION_WEB_H
