#ifndef WEBSERVER_FEATURE_HELPER_H
#define WEBSERVER_FEATURE_HELPER_H

#include <Arduino.h>
#include <IPAddress.h>

/**
 * @brief Generate the root page HTML
 * @param deviceId Device ID string
 * @param firmwareName Firmware name string
 * @param ipAddress IP address
 * @param uptimeSeconds Uptime in seconds
 * @param freeHeap Free heap in bytes
 * @return Complete HTML string for the root page
 */
String generateRootPageHtml(const String& deviceId, const String& firmwareName,
                           const IPAddress& ipAddress, uint32_t uptimeSeconds, uint32_t freeHeap);

/**
 * @brief Generate the storage browser page HTML
 * @return Complete HTML string for the storage browser page
 */
String generateStoragePageHtml();

#endif // WEBSERVER_FEATURE_HELPER_H