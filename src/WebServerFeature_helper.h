#ifndef WEBSERVER_FEATURE_HELPER_H
#define WEBSERVER_FEATURE_HELPER_H

#include <Arduino.h>
#include <IPAddress.h>

// Forward-declare to avoid pulling heavy AsyncWebServer headers into all units
class AsyncWebServerRequest;

namespace WebServerHelper {

/**
 * @brief Stream the root page directly to an AsyncWebServerRequest response.
 *
 * This avoids building a large `String` in RAM by printing static parts
 * from flash using `F(...)` and streaming the small dynamic parts.
 */
void sendRootPage(AsyncWebServerRequest* request, const String& deviceId, const String& firmwareName,
                  const IPAddress& ipAddress, uint32_t uptimeSeconds, uint32_t freeHeap);

/**
 * @brief Stream the storage browser page directly to an AsyncWebServerRequest.
 */
void sendStoragePage(AsyncWebServerRequest* request);

} // namespace WebServerHelper

#endif // WEBSERVER_FEATURE_HELPER_H