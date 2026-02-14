#ifndef DATA_COLLECTION_WEB_H
#define DATA_COLLECTION_WEB_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include "DataCollection.h"
#include "WebServerFeature.h"

/**
 * @file DataCollectionWeb.h
 * @brief Helper utilities to expose a DataCollection over HTTP
 *
 * This helper registers three endpoints per collection:
 * - `/api/<name>`       - JSON API returning all data
 * - `/api/<name>/latest`- JSON API returning the latest entry
 * - `/view/<name>`      - HTML table view with auto-refresh
 */
class DataCollectionWeb {
public:
    /**
     * @brief Register web endpoints for a data collection
     * @param server AsyncWebServer instance
     * @param basePath Base path for endpoints (e.g., "sensors" -> /api/sensors, /view/sensors)
     * @param getJsonCallback Callback returning JSON string of all data
     * @param getLatestJsonCallback Callback returning JSON string of the latest entry
     * @param getSchemaCallback Callback returning field names as a JSON array string
     * @param refreshIntervalMs Auto-refresh interval for HTML view (milliseconds, default 5000)
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
            streamHtmlView(request, basePath, apiPathStr, refreshIntervalMs);
        });
    }

    /**
     * @brief Register endpoints using WebServerFeature (enforces auth if enabled)
     *
     * Same parameters as the raw `registerEndpoints` overload but uses the
     * project's `WebServerFeature` to perform optional authentication checks.
     */
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
            streamHtmlView(request, basePath, apiPathStr, refreshIntervalMs);
        });
    }
    
    /**
     * @brief Convenience method to register endpoints for a DataCollection instance
     * @tparam T element type stored in the DataCollection
     * @tparam N capacity of the DataCollection
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
    /**
     * @brief Attempt to produce a JSON array of field/column names for the
     * given collection.
     *
     * Currently the project-level DataCollection schema is not exposed here,
     * so this returns an empty array. The HTML view will render without it.
     *
     * @return JSON array string of field names (e.g., "[\"time\",\"value\"]")
     */
    static String getFieldNames(DataCollection<T, N>& collection) {
        return "[]";
    }
    
    /**
     * @brief Stream an HTML table view for the collection to the client.
     *
     * @param request AsyncWebServerRequest to write the response to
     * @param name Human-readable collection name used in headings
     * @param apiPath API path this view will poll for JSON data
     * @param refreshIntervalMs Auto-refresh interval in milliseconds
     */
    static void streamHtmlView(AsyncWebServerRequest* request, const char* name, const char* apiPath, uint32_t refreshIntervalMs) {
        AsyncResponseStream *response = request->beginResponseStream(F("text/html"));
        response->print(F("<!DOCTYPE html>\n<html>\n<head>\n"
            "    <meta charset=\"UTF-8\">\n"
            "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
            "    <title>"));
        response->print(name);
        response->print(F(" - Data View</title>\n"
            "    <link rel=\"stylesheet\" href=\"/style.css\">\n"
            "    <style>\n"
            "        h1, a, .btn, button, th {color:#66BB6A!important}\n"
            "        .btn, button {background:#66BB6A!important}\n"
            "        .btn:hover, button:hover {background:#4CAF50!important}\n"
            "        .status{display:flex;gap:15px;margin-bottom:15px;flex-wrap:wrap}\n"
            "        .status-item{background:#16213e;padding:8px 12px;border-radius:6px;font-size:0.9em}\n"
            "        .status-item span{color:#66BB6A;font-weight:bold}\n"
            "        .status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px;animation:pulse 2s infinite}\n"
            "        .status-dot.connected{background:#00ff88}\n"
            "        .status-dot.disconnected{background:#ff4444}\n"
            "        @keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}\n"
            "        .latest{background:#1f4a3f!important}\n"
            "    </style>\n"
            "</head>\n<body>\n"
            "    <div class=\"container\">\n"
            "        <a href=\"/\" class=\"home-link\">Home</a>\n"
            "        <h1>"));
        response->print(name);
        response->print(F("</h1>\n"
            "        <div class=\"status\">\n"
            "            <div class=\"status-item\">\n"
            "                <span class=\"status-dot connected\" id=\"statusDot\"></span>\n"
            "                <span id=\"statusText\">Connected</span>\n"
            "            </div>\n"
            "            <div class=\"status-item\">Entries: <span id=\"entryCount\">0</span></div>\n"
            "            <div class=\"status-item\">Last Update: <span id=\"lastUpdate\">-</span></div>\n"
            "            <button class=\"btn\" onclick=\"fetchData()\">Refresh Now</button>\n"
            "        </div>\n"
            "        <div class=\"table-container\">\n"
            "            <table id=\"dataTable\">\n"
            "                <thead id=\"tableHead\"></thead>\n"
            "                <tbody id=\"tableBody\"></tbody>\n"
            "            </table>\n"
            "            <div class=\"no-data\" id=\"noData\" style=\"display:none;\">No data available</div>\n"
            "        </div>\n"
            "        <div class=\"status-info\">Auto-refresh every "));
        response->print(refreshIntervalMs / 1000);
        response->print(F(" seconds</div>\n"
            "    </div>\n\n"
            "    <script>\n"
            "        const API_URL = '"));
        response->print(apiPath);
        response->print(F("';\n"
            "        const REFRESH_INTERVAL = "));
        response->print(refreshIntervalMs);
        response->print(F(";\n"
            "        \n"
            "        let columns = [];\n"
            "        let lastData = null;\n"
            "        \n"
            "        function formatValue(key, value) {\n"
            "            if (value === null || value === undefined) return '-';\n"
            "            if (key === 'timestamp' || key.includes('time')) {\n"
            "                if (typeof value === 'number' && value > 1000000000) {\n"
            "                    const date = new Date(value * 1000);\n"
            "                    return date.toLocaleString();\n"
            "                }\n"
            "            }\n"
            "            if (typeof value === 'number') {\n"
            "                if (Number.isInteger(value)) return value.toString();\n"
            "                return value.toFixed(2);\n"
            "            }\n"
            "            if (typeof value === 'boolean') return value ? 'Yes' : 'No';\n"
            "            return String(value);\n"
            "        }\n"
            "        \n"
            "        function updateTable(data) {\n"
            "            const thead = document.getElementById('tableHead');\n"
            "            const tbody = document.getElementById('tableBody');\n"
            "            const noData = document.getElementById('noData');\n"
            "            const entryCount = document.getElementById('entryCount');\n"
            "            \n"
            "            if (!data || data.length === 0) {\n"
            "                thead.innerHTML = '';\n"
            "                tbody.innerHTML = '';\n"
            "                noData.style.display = 'block';\n"
            "                entryCount.textContent = '0';\n"
            "                return;\n"
            "            }\n"
            "            \n"
            "            noData.style.display = 'none';\n"
            "            entryCount.textContent = data.length;\n"
            "            \n"
            "            if (columns.length === 0 && data.length > 0) {\n"
            "                columns = Object.keys(data[0]);\n"
            "            }\n"
            "            \n"
            "            thead.innerHTML = '<tr>' + columns.map(col => \n"
            "                `<th>${col}</th>`\n"
            "            ).join('') + '</tr>';\n"
            "            \n"
            "            const reversedData = [...data].reverse();\n"
            "            tbody.innerHTML = reversedData.map((row, idx) => \n"
            "                `<tr class=\"${idx === 0 ? 'latest' : ''}\">${columns.map(col => \n"
            "                    `<td>${formatValue(col, row[col])}</td>`\n"
            "                ).join('')}</tr>`\n"
            "            ).join('');\n"
            "        }\n"
            "        \n"
            "        async function fetchData() {\n"
            "            try {\n"
            "                const response = await fetch(API_URL);\n"
            "                if (!response.ok) throw new Error('HTTP ' + response.status);\n"
            "                \n"
            "                const data = await response.json();\n"
            "                lastData = data;\n"
            "                updateTable(data);\n"
            "                \n"
            "                document.getElementById('statusDot').className = 'status-dot connected';\n"
            "                document.getElementById('statusText').textContent = 'Connected';\n"
            "                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();\n"
            "            } catch (error) {\n"
            "                console.error('Fetch error:', error);\n"
            "                document.getElementById('statusDot').className = 'status-dot disconnected';\n"
            "                document.getElementById('statusText').textContent = 'Disconnected';\n"
            "            }\n"
            "        }\n"
            "        \n"
            "        fetchData();\n"
            "        setInterval(fetchData, REFRESH_INTERVAL);\n"
            "    </script>\n"
            "</body>\n</html>"));
        request->send(response);
    }
};

#endif // DATA_COLLECTION_WEB_H
