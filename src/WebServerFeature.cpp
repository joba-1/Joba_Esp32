#include "WebServerFeature.h"
#include "DeviceInfo.h"
#include "LoggingFeature.h"
#include <WiFi.h>

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
