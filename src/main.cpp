#include <Arduino.h>
#include "LoggingFeature.h"
#include "WiFiManagerFeature.h"
#include "TimeSyncFeature.h"
#include "WebServerFeature.h"
#include "OTAFeature.h"

// Feature instances - parameters come from platformio.ini build_flags
LoggingFeature logging(LOG_BAUD_RATE, LOG_LEVEL, LOG_ENABLE_TIMESTAMP);
WiFiManagerFeature wifiManager(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_CONFIG_PORTAL_TIMEOUT);
TimeSyncFeature timeSync(NTP_SERVER1, NTP_SERVER2, TIMEZONE, NTP_SYNC_INTERVAL);
WebServerFeature webServer(WEBSERVER_PORT, WEBSERVER_USERNAME, WEBSERVER_PASSWORD);
OTAFeature ota(OTA_HOSTNAME, OTA_PASSWORD, OTA_PORT);

// Array of all features for easy iteration
Feature* features[] = {
    &logging,      // Must be first for early logging
    &wifiManager,  // Must be before network-dependent features
    &timeSync,
    &webServer,
    &ota
};
const size_t featureCount = sizeof(features) / sizeof(features[0]);

void setup() {
    // Initialize all features
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->setup();
        LOG_I("Feature '%s' setup complete", features[i]->getName());
    }
    
    LOG_I("All features initialized");
    LOG_I("Free heap: %d bytes", ESP.getFreeHeap());
}

void loop() {
    // Run all feature loop handlers
    for (size_t i = 0; i < featureCount; i++) {
        features[i]->loop();
    }
}
