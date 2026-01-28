#ifndef WEBSERVER_FEATURE_H
#define WEBSERVER_FEATURE_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "Feature.h"

/**
 * @brief Async web server for REST API and web interface
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
     * @brief Get access to the underlying AsyncWebServer
     */
    AsyncWebServer* getServer();
    
    /**
     * @brief Add a handler to the server
     */
    void addHandler(AsyncWebHandler* handler);
    
    /**
     * @brief Register a route handler
     */
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
    
    /**
     * @brief Check if authentication is required and valid
     */
    bool authenticate(AsyncWebServerRequest* request);
    
    /**
     * @brief Set password for basic auth
     */
    void setPassword(const char* password) { 
        _password = password;
        _authEnabled = (strlen(_username) > 0 && strlen(_password) > 0);
    }

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
