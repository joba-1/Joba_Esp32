#ifndef MODBUS_WEB_H
#define MODBUS_WEB_H

#include "ModbusDevice.h"
#include "ModbusRTUFeature.h"
#include "WebServerFeature.h"
#include <ArduinoJson.h>
#include "TimeUtils.h"

/**
 * @brief Web interface for Modbus devices
 * 
 * Provides REST API endpoints for:
 * - Listing devices and their values
 * - Reading/writing registers
 * - Viewing raw bus data
 * - Device type management
 */
class ModbusWeb {
public:
    /**
     * @brief Initialize Modbus web endpoints
     * @param server WebServer feature
     * @param modbus Low-level Modbus RTU feature
     * @param devices Device manager
     */
    static void setup(WebServerFeature& server, ModbusRTUFeature& modbus,
                      ModbusDeviceManager& devices) {
        auto* webServer = server.getServer();
        
        // List all configured devices
        webServer->on("/api/modbus/devices", HTTP_GET,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                auto _guard = devices.scopedLock();

                // Debug endpoint: avoid fixed-capacity docs to prevent silent member drops.
                JsonDocument doc;
                JsonArray arr = doc.to<JsonArray>();
                
                for (const auto& kv : devices.getDevices()) {
                    JsonObject dev = arr.add<JsonObject>();
                    dev["unitId"] = kv.first;
                    dev["type"] = kv.second.deviceTypeName;
                    dev["successCount"] = kv.second.successCount;
                    dev["errorCount"] = kv.second.errorCount;
                    dev["valuesCount"] = (uint32_t)kv.second.currentValues.size();
                    dev["unknownCount"] = (uint32_t)kv.second.unknownU16.size();
                }
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Get device values
        webServer->on("/api/modbus/device", HTTP_GET,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                if (!request->hasParam("unit")) {
                    request->send(400, "application/json", "{\"error\":\"Missing unit parameter\"}");
                    return;
                }
                
                uint8_t unitId = request->getParam("unit")->value().toInt();
                auto* response = request->beginResponseStream("application/json");
                if (request->hasParam("meta")) {
                    devices.writeDeviceMetaJson(unitId, *response);
                } else {
                    devices.writeDeviceValuesJson(unitId, *response);
                }
                request->send(response);
            });
        
        // Read a specific register
        webServer->on("/api/modbus/read", HTTP_GET,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

#if MODBUS_LISTEN_ONLY
                request->send(409, "application/json",
                              "{\"error\":\"Modbus is in listen-only mode (sending disabled)\"}");
                return;
#endif

                if (!request->hasParam("unit") || !request->hasParam("register")) {
                    request->send(400, "application/json",
                                  "{\"error\":\"Missing unit or register parameter\"}");
                    return;
                }
                
                uint8_t unitId = request->getParam("unit")->value().toInt();
                String regName = request->getParam("register")->value();
                
                // Queue the read
                bool queued = devices.readRegister(unitId, regName.c_str(), nullptr);
                
                // Return current cached value (or stale)
                float value = 0;
                bool valid = devices.getValue(unitId, regName.c_str(), value);
                
                JsonDocument doc;
                doc["unitId"] = unitId;
                doc["register"] = regName;
                doc["value"] = value;
                doc["valid"] = valid;
                doc["queued"] = queued;
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Write to a register
        webServer->on("/api/modbus/write", HTTP_POST,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

#if MODBUS_LISTEN_ONLY
                request->send(409, "application/json",
                              "{\"error\":\"Modbus is in listen-only mode (sending disabled)\"}");
                return;
#endif

                if (!request->hasParam("unit", true) ||
                    !request->hasParam("register", true) ||
                    !request->hasParam("value", true)) {
                    request->send(400, "application/json",
                                  "{\"error\":\"Missing unit, register or value parameter\"}");
                    return;
                }
                
                uint8_t unitId = request->getParam("unit", true)->value().toInt();
                String regName = request->getParam("register", true)->value();
                float value = request->getParam("value", true)->value().toFloat();
                
                bool queued = devices.writeRegister(unitId, regName.c_str(), value, nullptr);
                
                JsonDocument doc;
                doc["unitId"] = unitId;
                doc["register"] = regName;
                doc["value"] = value;
                doc["queued"] = queued;
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Raw read request
        webServer->on("/api/modbus/raw/read", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

#if MODBUS_LISTEN_ONLY
                request->send(409, "application/json",
                              "{\"error\":\"Modbus is in listen-only mode (sending disabled)\"}");
                return;
#endif

                if (!request->hasParam("unit") ||
                    !request->hasParam("address") ||
                    !request->hasParam("count")) {
                    request->send(400, "application/json",
                                  "{\"error\":\"Missing unit, address or count parameter\"}");
                    return;
                }
                
                uint8_t unitId = request->getParam("unit")->value().toInt();
                uint16_t address = request->getParam("address")->value().toInt();
                uint16_t count = request->getParam("count")->value().toInt();
                uint8_t fc = request->hasParam("fc") ?
                             request->getParam("fc")->value().toInt() : 3;

                bool queued = modbus.queueReadRegisters(unitId, fc, address, count, nullptr);
                
                JsonDocument doc;
                doc["unitId"] = unitId;
                doc["address"] = address;
                doc["count"] = count;
                doc["functionCode"] = fc;
                doc["queued"] = queued;
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Get bus status
        webServer->on("/api/modbus/status", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                // Debug endpoint: avoid fixed-capacity docs to prevent silent member drops.
                JsonDocument doc;
                doc["listenOnly"] = (bool)MODBUS_LISTEN_ONLY;
                doc["busSilent"] = modbus.isBusSilent();
                doc["silenceMs"] = modbus.getTimeSinceLastActivity();
                doc["queuedRequests"] = modbus.getQueuedRequestCount();
                doc["inFlightRequest"] = modbus.isWaitingForResponse();
                doc["pendingRequests"] = modbus.getPendingRequestCount();
                doc["rxFrames"] = modbus.getStats().framesReceived;
                doc["txFrames"] = modbus.getStats().framesSent;
                doc["crcErrors"] = modbus.getStats().crcErrors;
                doc["ownRequestsSent"] = modbus.getStats().ownRequestsSent;
                doc["ownRequestsSuccess"] = modbus.getStats().ownRequestsSuccess;
                doc["ownRequestsFailed"] = modbus.getStats().ownRequestsFailed;
                doc["ownRequestsDiscarded"] = modbus.getStats().ownRequestsDiscarded;
                doc["consecutiveTimeouts"] = modbus.getConsecutiveTimeouts();
                doc["queueingPaused"] = modbus.isQueueingPaused();
                doc["queueingPauseRemainingMs"] = modbus.getQueueingPauseRemainingMs();
                doc["queueingBackoffMs"] = modbus.getQueueingBackoffMs();

                JsonArray unitBackoff = doc["unitBackoff"].to<JsonArray>();
                for (const auto& info : modbus.getUnitBackoffInfo()) {
                    JsonObject o = unitBackoff.add<JsonObject>();
                    o["unitId"] = info.unitId;
                    o["consecutiveTimeouts"] = info.consecutiveTimeouts;
                    o["backoffMs"] = info.backoffMs;
                    o["pausedUntilMs"] = info.pausedUntilMs;
                    o["paused"] = info.paused;
                    o["pauseRemainingMs"] = info.pauseRemainingMs;
                }

                JsonObject debug = doc["debug"].to<JsonObject>();
                debug["sinceLastByteUs"] = modbus.getTimeSinceLastByteUs();
                debug["charTimeUs"] = modbus.getCharTimeUs();
                debug["silenceTimeUs"] = modbus.getSilenceTimeUs();
                debug["loopCounter"] = modbus.getLoopCounter();
                debug["processQueueCounter"] = modbus.getProcessQueueCounter();
                debug["lastProcessQueueMs"] = (uint32_t)modbus.getLastProcessQueueMs();
                debug["dbgQueueSizeInLoop"] = modbus.getDbgQueueSizeInLoop();
                debug["dbgWaitingForResponseInLoop"] = modbus.getDbgWaitingForResponseInLoop();
                debug["dbgSerialAvailableInLoop"] = modbus.getDbgSerialAvailableInLoop();
                debug["dbgRxBytesDrainedInLoop"] = modbus.getDbgRxBytesDrainedInLoop();
                debug["dbgGapUsInLoop"] = modbus.getDbgGapUsInLoop();
                debug["dbgGapEnoughForTxInLoop"] = modbus.getDbgGapEnoughForTxInLoop();
                debug["dbgLastLoopSnapshotMs"] = (uint32_t)modbus.getDbgLastLoopSnapshotMs();

                JsonObject updated = doc["updated"].to<JsonObject>();
                updated["uptimeMs"] = (uint32_t)millis();
                const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
                if (nowUnix != 0) {
                    updated["epoch"] = nowUnix;
                    String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
                    if (iso.length() > 0) updated["iso"] = iso;
                }
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Get register maps from bus monitoring
        webServer->on("/api/modbus/maps", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                const bool timeValid = TimeUtils::isTimeValidNow();
                const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
                const unsigned long nowMs = millis();

                JsonDocument doc;
                JsonArray maps = doc.to<JsonArray>();
                
                for (const auto& kv : modbus.getAllRegisterMaps()) {
                    JsonObject map = maps.add<JsonObject>();
                    map["unitId"] = kv.second.unitId;
                    map["functionCode"] = kv.second.functionCode;

                    // Group update time fields under a common pattern.
                    JsonObject updated = map["updated"].to<JsonObject>();
                    updated["uptimeMs"] = (uint32_t)kv.second.lastUpdate;

                    if (kv.second.lastUpdate != 0 && timeValid && nowUnix != 0) {
                        uint32_t ageMs = (uint32_t)(nowMs - kv.second.lastUpdate);
                        uint32_t estEpoch = nowUnix - (ageMs / 1000);
                        updated["epoch"] = estEpoch;
                        String iso = TimeUtils::isoUtcFromUnixSeconds(estEpoch);
                        if (iso.length() > 0) updated["iso"] = iso;
                    }
                    map["requestCount"] = kv.second.requestCount;
                    map["responseCount"] = kv.second.responseCount;
                    map["errorCount"] = kv.second.errorCount;
                    
                    // Include register values
                    JsonArray regs = map["registers"].to<JsonArray>();
                    for (const auto& regKv : kv.second.registers) {
                        JsonObject reg = regs.add<JsonObject>();
                        reg["address"] = regKv.first;
                        reg["value"] = regKv.second;
                    }
                }
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // Device types list
        webServer->on("/api/modbus/types", HTTP_GET,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                JsonDocument doc;
                JsonArray arr = doc.to<JsonArray>();
                
                for (const auto& name : devices.getDeviceTypeNames()) {
                    arr.add(name);
                }
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // HTML dashboard
        webServer->on("/view/modbus", HTTP_GET,
            [&devices, &modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                String html = F("<!DOCTYPE html><html><head>"
                    "<title>Modbus Dashboard</title>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<style>"
                    "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
                    ".card{background:white;border-radius:8px;padding:15px;margin:10px 0;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
                    ".device{border-left:4px solid #2196F3}"
                    ".status{border-left:4px solid #4CAF50}"
                    "table{width:100%;border-collapse:collapse}"
                    "th,td{padding:8px;text-align:left;border-bottom:1px solid #ddd}"
                    "th{background:#f9f9f9}"
                    ".ok{color:#4CAF50}.err{color:#F44336}"
                    "h1{color:#333}h2{color:#666;margin:0 0 10px 0}"
                    "</style></head><body>"
                    "<h1>Modbus Dashboard</h1>");
                
                // Bus status
                html += F("<div class='card status'><h2>Bus Status</h2>");
                html += F("<p>Silent: ");
                html += modbus.isBusSilent() ? F("<span class='ok'>Yes</span>") : F("<span class='err'>No</span>");
                html += F(" | Queue: ");
                html += String(modbus.getPendingRequestCount());
                html += F(" | RX: ");
                html += String(modbus.getStats().framesReceived);
                html += F(" | TX: ");
                html += String(modbus.getStats().framesSent);
                html += F(" | CRC Errors: ");
                html += String(modbus.getStats().crcErrors);
                html += F("</p></div>");
                
                // Devices
                for (const auto& kv : devices.getDevices()) {
                    const auto& dev = kv.second;
                    html += F("<div class='card device'><h2>Unit ");
                    html += String(dev.unitId);
                    html += F(" - ");
                    html += dev.deviceTypeName;
                    html += F("</h2>");
                    html += F("<p>Success: ");
                    html += String(dev.successCount);
                    html += F(" | Errors: ");
                    html += String(dev.errorCount);
                    html += F("</p><table><tr><th>Register</th><th>Value</th><th>Unit</th><th>Valid</th></tr>");
                    
                    for (const auto& val : dev.currentValues) {
                        html += F("<tr><td>");
                        html += val.second.name;
                        html += F("</td><td>");
                        html += String(val.second.value, 2);
                        html += F("</td><td>");
                        html += val.second.unit;
                        html += F("</td><td class='");
                        html += val.second.valid ? F("ok'>✓") : F("err'>✗");
                        html += F("</td></tr>");
                    }
                    
                    html += F("</table></div>");
                }
                
                html += F("<script>setTimeout(()=>location.reload(),5000)</script>");
                html += F("</body></html>");
                
                request->send(200, "text/html", html);
            });
        
        // Modbus monitoring data
        webServer->on("/api/modbus/monitor", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                JsonDocument doc;
                doc["busSilent"] = modbus.isBusSilent();
                doc["silenceMs"] = modbus.getTimeSinceLastActivity();
                doc["minSilenceUs"] = modbus.getMinSilenceTimeUs();

                {
                    JsonObject updated = doc["updated"].to<JsonObject>();
                    updated["uptimeMs"] = (uint32_t)millis();
                    const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
                    if (nowUnix != 0) {
                        updated["epoch"] = nowUnix;
                        String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
                        if (iso.length() > 0) updated["iso"] = iso;
                    }
                }

                JsonArray frames = doc["recentFrames"].to<JsonArray>();
                // need to implement using the frame history buffer in ModbusRTUFeature
                for (const auto& frame : modbus.getRecentFrames()) {
                    JsonObject f = frames.add<JsonObject>();

                    JsonObject updated = f["updated"].to<JsonObject>();
                    updated["uptimeMs"] = (uint32_t)frame.timestamp;
                    if (frame.unixTimestamp != 0) {
                        updated["epoch"] = frame.unixTimestamp;
                        String iso = TimeUtils::isoUtcFromUnixSeconds(frame.unixTimestamp);
                        if (iso.length() > 0) updated["iso"] = iso;
                    }
                    f["unitId"] = frame.unitId;
                    f["functionCode"] = frame.functionCode;
                    f["isRequest"] = frame.isRequest;
                    f["valid"] = frame.isValid;
                    f["crc"] = frame.crc;
                    {
                        char crcHex[7];
                        snprintf(crcHex, sizeof(crcHex), "0x%04X", (unsigned)frame.crc);
                        f["crcHex"] = crcHex;
                    }

                    uint8_t fc = frame.functionCode & 0x7F;
                    f["functionCodeBase"] = fc;
                    f["isException"] = frame.isException;
                    if (frame.isException) {
                        f["exceptionCode"] = frame.exceptionCode;
                    }

                    // Keep the previous raw payload as hex for debugging.
                    f["dataHex"] = modbus.formatHex(frame.data.data(), frame.data.size());

                    // Split out common Modbus RTU FC3/FC4 fields.
                    if (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS) {
                        if (frame.isRequest && frame.data.size() == 4) {
                            f["startRegister"] = frame.getStartRegister();
                            f["quantity"] = frame.getQuantity();
                        } else if (!frame.isRequest && !frame.isException && frame.data.size() >= 1) {
                            uint32_t byteCount = (uint32_t)frame.getByteCount();
                            f["byteCount"] = byteCount;

                            const uint8_t* regData = frame.getRegisterData();
                            if (regData && byteCount >= 2) {
                                // Hex dump of register payload only (no byteCount field)
                                f["registerDataHex"] = modbus.formatHex(regData, byteCount);

                                // Also provide a bounded words array for convenience
                                JsonArray words = f["registerWords"].to<JsonArray>();
                                size_t wordCount = (size_t)byteCount / 2;
                                static constexpr size_t MAX_WORDS = 32;
                                size_t emitCount = wordCount > MAX_WORDS ? MAX_WORDS : wordCount;
                                for (size_t i = 0; i < emitCount; i++) {
                                    size_t idx = i * 2;
                                    uint16_t w = ((uint16_t)regData[idx] << 8) | (uint16_t)regData[idx + 1];
                                    words.add(w);
                                }
                                if (wordCount > MAX_WORDS) {
                                    f["registerWordsTruncated"] = true;
                                    f["registerWordCount"] = (uint32_t)wordCount;
                                }
                            }
                        }
                    }
                }

                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
    }
};

#endif // MODBUS_WEB_H
