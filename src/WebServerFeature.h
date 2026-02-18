#ifndef WEBSERVER_FEATURE_H
#define WEBSERVER_FEATURE_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "Feature.h"

/**
 * @brief Async web server feature
 *
 * Hosts the REST API endpoints and several human-friendly HTML pages used for
 * debugging and monitoring. Uses `ESPAsyncWebServer` and supports optional
 * basic authentication.
 */
class WebServerFeature : public Feature {
public:
    /**
     * @brief Construct WebServer feature
     * @param port HTTP server port
     * @param username Basic auth username (empty = no auth)
     * @param password Basic auth password
     */
    WebServerFeature(uint16_t port, const char* username, const char* password);
    
    void setup() override;
    const char* getName() const override { return "WebServer"; }
    bool isReady() const override { return _ready; }
    
    /**
     * @brief Get access to the underlying `AsyncWebServer`
     * @return Pointer to the server instance (nullptr until `setup()` runs)
     */
    AsyncWebServer* getServer();
    
    /**
     * @brief Add a pre-built handler to the server
     * @param handler Pointer to an `AsyncWebHandler` instance
     */
    void addHandler(AsyncWebHandler* handler);
    
    /**
     * @brief Register a route handler
     * @param uri Request path (e.g., "/api/status")
     * @param method HTTP methods to accept
     * @param onRequest Handler function invoked for matching requests
     */
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
    
    /**
     * @brief Check basic auth credentials on the request
     * @param request Request to authenticate
     * @return true when auth is not required or credentials are valid
     */
    bool authenticate(AsyncWebServerRequest* request);
    
    /**
     * @brief Set password for basic auth
     * @param password NUL-terminated password string
     */
    void setPassword(const char* password) { 
        _password = password;
        _authEnabled = (strlen(_username) > 0 && strlen(_password) > 0);
    }

    /**
     * @brief Safely serialize and send a JSON document as an HTTP response.
     *
     * Measures the serialization size with `measureJson()` and returns HTTP
     * 503 when the payload would exceed `maxBytes` to avoid exhausting the
     * async response buffer or heap. Implemented in `WebServerFeature.cpp`.
     */
    static void safeSendJson(AsyncWebServerRequest* request, JsonDocument& doc, size_t maxBytes = 8192, int statusCode = 200);
    /**
     * @brief Record the request path as the last response-starting path.
     *
     * Call this right before creating/starting a response that may allocate
     * significant buffers so the path is available in logs if allocation
     * failures occur inside the async response machinery.
     */
    static void noteResponse(AsyncWebServerRequest* request);

    /**
     * @brief Ensure there's sufficient free heap to start a streamed response.
     * @return true when it's safe to start the response, false and a 503 sent otherwise.
     */
    static bool ensureHeapForResponse(AsyncWebServerRequest* request, size_t minFreeHeap = 20000);

private:
    void setupDefaultRoutes();
    
    uint16_t _port;
    const char* _username;
    const char* _password;
    bool _authEnabled;
    bool _ready;
    bool _setupDone;
    
    AsyncWebServer* _server;
};

#endif // WEBSERVER_FEATURE_H
