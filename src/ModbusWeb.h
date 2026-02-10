#ifndef MODBUS_WEB_H
#define MODBUS_WEB_H

#include "ModbusDevice.h"
#include "ModbusRTUFeature.h"
#include "WebServerFeature.h"
#include <ArduinoJson.h>
#include <map>
#include <cmath>
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

        struct TrackedRawReadResult {
            uint32_t id{0};
            uint32_t createdMs{0};
            uint32_t completedMs{0};
            uint8_t unitId{0};
            uint8_t functionCode{0};
            uint16_t address{0};
            uint16_t count{0};
            bool queued{false};
            bool completed{false};
            bool success{false};
            bool isException{false};
            uint8_t exceptionCode{0};
            uint16_t crc{0};
            String dataHex;
            String registerDataHex;
            std::vector<uint16_t> words;
        };

        static std::map<uint32_t, TrackedRawReadResult> s_trackedRawReads;
        static uint32_t s_nextTrackedId = 1;

        auto purgeTracked = [&]() {
            static constexpr uint32_t MAX_AGE_MS = 5UL * 60UL * 1000UL;
            static constexpr size_t MAX_ITEMS = 32;
            const uint32_t nowMs = (uint32_t)millis();

            // Age-based purge
            for (auto it = s_trackedRawReads.begin(); it != s_trackedRawReads.end();) {
                if ((uint32_t)(nowMs - it->second.createdMs) > MAX_AGE_MS) {
                    it = s_trackedRawReads.erase(it);
                } else {
                    ++it;
                }
            }

            // Size-based purge (oldest first)
            while (s_trackedRawReads.size() > MAX_ITEMS) {
                auto oldestIt = s_trackedRawReads.begin();
                for (auto it = s_trackedRawReads.begin(); it != s_trackedRawReads.end(); ++it) {
                    if (it->second.createdMs < oldestIt->second.createdMs) oldestIt = it;
                }
                s_trackedRawReads.erase(oldestIt);
            }
        };
        
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
                ModbusRegisterValue regValue;
                bool hasValue = devices.getRegisterValue(unitId, regName.c_str(), regValue);
                
                JsonDocument doc;
                doc["unitId"] = unitId;
                doc["register"] = regName;
                doc["queued"] = queued;
                
                if (hasValue) {
                    doc["value"] = regValue.value;
                    doc["valid"] = regValue.valid;
                    doc["unit"] = regValue.unit;
                    doc["timestamp"] = regValue.timestamp;
                    doc["updatedAtMs"] = regValue.updatedAtMs;
                    if (regValue.unixTimestamp != 0) {
                        doc["unixTimestamp"] = regValue.unixTimestamp;
                    }
                    // Calculate age in seconds
                    uint32_t nowMs = (uint32_t)millis();
                    uint32_t ageMs = nowMs - regValue.updatedAtMs;
                    doc["ageSeconds"] = ageMs / 1000;
                } else {
                    doc["value"] = 0;
                    doc["valid"] = false;
                }
                
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

        // Raw read request (tracked) - returns a requestId that can be polled via /api/modbus/raw/result
        webServer->on("/api/modbus/raw/readTracked", HTTP_GET,
            [&modbus, &server, purgeTracked](AsyncWebServerRequest* request) mutable {
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

                purgeTracked();

                uint8_t unitId = request->getParam("unit")->value().toInt();
                uint16_t address = request->getParam("address")->value().toInt();
                uint16_t count = request->getParam("count")->value().toInt();
                uint8_t fc = request->hasParam("fc") ?
                             request->getParam("fc")->value().toInt() : 3;

                uint32_t requestId = s_nextTrackedId++;
                if (requestId == 0) requestId = s_nextTrackedId++;

                TrackedRawReadResult st;
                st.id = requestId;
                st.createdMs = (uint32_t)millis();
                st.unitId = unitId;
                st.functionCode = fc;
                st.address = address;
                st.count = count;

                // Install state before queueing so the callback can always find it.
                s_trackedRawReads[requestId] = st;

                bool queued = modbus.queueReadRegisters(
                    unitId, fc, address, count,
                    [&modbus, requestId](bool success, const ModbusFrame& response) {
                        auto it = s_trackedRawReads.find(requestId);
                        if (it == s_trackedRawReads.end()) return;
                        TrackedRawReadResult& r = it->second;
                        r.completed = true;
                        r.completedMs = (uint32_t)millis();
                        r.success = success;
                        r.isException = response.isException;
                        r.exceptionCode = response.exceptionCode;
                        r.crc = response.crc;

                        r.dataHex = modbus.formatHex(response.data.data(), response.dataLen);

                        // FC3/FC4 response: byteCount + payload
                        uint8_t fcBase = response.functionCode & 0x7F;
                        if (!response.isException && (fcBase == ModbusFC::READ_HOLDING_REGISTERS || fcBase == ModbusFC::READ_INPUT_REGISTERS)) {
                            const size_t byteCount = response.getByteCount();
                            const uint8_t* regData = response.getRegisterData();
                            if (regData && byteCount >= 2) {
                                r.registerDataHex = modbus.formatHex(regData, byteCount);
                                r.words.clear();
                                size_t wordCount = byteCount / 2;
                                static constexpr size_t MAX_WORDS = 32;
                                size_t emitCount = wordCount > MAX_WORDS ? MAX_WORDS : wordCount;
                                r.words.reserve(emitCount);
                                for (size_t i = 0; i < emitCount; i++) {
                                    size_t idx = i * 2;
                                    uint16_t w = ((uint16_t)regData[idx] << 8) | (uint16_t)regData[idx + 1];
                                    r.words.push_back(w);
                                }
                            }
                        }
                    });

                s_trackedRawReads[requestId].queued = queued;

                JsonDocument doc;
                doc["requestId"] = requestId;
                doc["queued"] = queued;
                doc["unitId"] = unitId;
                doc["address"] = address;
                doc["count"] = count;
                doc["functionCode"] = fc;

                String output;
                serializeJson(doc, output);
                request->send(queued ? 200 : 503, "application/json", output);
            });

        // Fetch tracked raw read result
        webServer->on("/api/modbus/raw/result", HTTP_GET,
            [&server, purgeTracked](AsyncWebServerRequest* request) mutable {
                if (!server.authenticate(request)) return request->requestAuthentication();

                if (!request->hasParam("id")) {
                    request->send(400, "application/json", "{\"error\":\"Missing id parameter\"}");
                    return;
                }

                purgeTracked();

                uint32_t id = (uint32_t)request->getParam("id")->value().toInt();
                auto it = s_trackedRawReads.find(id);
                if (it == s_trackedRawReads.end()) {
                    request->send(404, "application/json", "{\"error\":\"Unknown request id\"}");
                    return;
                }

                const TrackedRawReadResult& r = it->second;
                JsonDocument doc;
                doc["requestId"] = r.id;
                doc["queued"] = r.queued;
                doc["completed"] = r.completed;
                doc["success"] = r.success;
                doc["unitId"] = r.unitId;
                doc["address"] = r.address;
                doc["count"] = r.count;
                doc["functionCode"] = r.functionCode;
                doc["isException"] = r.isException;
                if (r.isException) doc["exceptionCode"] = r.exceptionCode;
                doc["createdMs"] = r.createdMs;
                if (r.completed) doc["completedMs"] = r.completedMs;
                if (r.completed) {
                    doc["crc"] = r.crc;
                    {
                        char crcHex[7];
                        snprintf(crcHex, sizeof(crcHex), "0x%04X", (unsigned)r.crc);
                        doc["crcHex"] = crcHex;
                    }
                    if (r.dataHex.length() > 0) doc["dataHex"] = r.dataHex;
                    if (r.registerDataHex.length() > 0) doc["registerDataHex"] = r.registerDataHex;
                    if (!r.words.empty()) {
                        JsonArray words = doc["registerWords"].to<JsonArray>();
                        for (uint16_t w : r.words) words.add(w);
                    }
                }

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

                doc["otherRequestsSeen"] = modbus.getStats().otherRequestsSeen;
                doc["otherResponsesSeen"] = modbus.getStats().otherResponsesSeen;
                doc["otherExceptionsSeen"] = modbus.getStats().otherExceptionsSeen;

                JsonObject otherPairing = doc["otherPairing"].to<JsonObject>();
                otherPairing["responsesPaired"] = modbus.getStats().otherResponsesPaired;
                otherPairing["responsesUnpaired"] = modbus.getStats().otherResponsesUnpaired;
                otherPairing["exceptionsPaired"] = modbus.getStats().otherExceptionsPaired;
                otherPairing["exceptionsUnpaired"] = modbus.getStats().otherExceptionsUnpaired;
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

        // Set silence time for arbitration testing
        // GET /api/modbus/silence?us=3000 to set 3000us
        // GET /api/modbus/silence?us=0 to reset to default
        // GET /api/modbus/silence to get current value
        webServer->on("/api/modbus/silence", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                if (request->hasParam("us")) {
                    uint32_t us = request->getParam("us")->value().toInt();
                    modbus.setSilenceTimeUs(us);
                }

                JsonDocument doc;
                doc["silenceTimeUs"] = modbus.getSilenceTimeUs();
                doc["charTimeUs"] = modbus.getCharTimeUs();
                doc["charTimes"] = (float)modbus.getSilenceTimeUs() / modbus.getCharTimeUs();
                doc["specMinCharTimes"] = 3.5;
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });

        // Reset stats for clean testing
        webServer->on("/api/modbus/stats/reset", HTTP_POST,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();
                modbus.resetStats();
                request->send(200, "application/json", "{\"reset\":true}");
            });

        // Recent CRC error contexts (before/bad/after) with full hex dumps
        auto handleModbusCrc = [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                uint32_t limit = 10;
                if (request->hasParam("limit")) {
                    limit = (uint32_t)request->getParam("limit")->value().toInt();
                    if (limit > 50) limit = 50;
                }

                size_t count = 0;
                const ModbusRTUFeature::CrcErrorContext* ctxs = modbus.getRecentCrcErrorContexts(count);

                JsonDocument doc;
                JsonObject updated = doc["updated"].to<JsonObject>();
                updated["uptimeMs"] = (uint32_t)millis();

                JsonArray items = doc["items"].to<JsonArray>();

                // Emit contexts in descending id (best-effort; small N so O(N^2) is fine).
                uint32_t lastId = 0xFFFFFFFF;
                for (uint32_t emitted = 0; emitted < limit; emitted++) {
                    const ModbusRTUFeature::CrcErrorContext* best = nullptr;
                    for (size_t i = 0; i < count; i++) {
                        const auto& c = ctxs[i];
                        if (c.id == 0) continue;
                        if (c.id >= lastId) continue;
                        if (!best || c.id > best->id) best = &c;
                    }
                    if (!best) break;
                    lastId = best->id;

                    JsonObject item = items.add<JsonObject>();
                    item["id"] = best->id;

                    auto writeFrame = [&](JsonObject obj, const ModbusFrame& f) {
                        obj["startUptimeMs"] = (uint32_t)f.timestamp;
                        obj["unitId"] = f.unitId;
                        obj["functionCode"] = f.functionCode;
                        obj["frameType"] = f.isRequest ? "request" : "response";
                        obj["isValid"] = f.isValid;
                        obj["isException"] = f.isException;
                        if (f.isException) obj["exceptionCode"] = f.exceptionCode;
                        {
                            const uint16_t calculatedCrc = modbus.calculateFrameCrc(f);

                            char crcReceivedHex[7];
                            snprintf(crcReceivedHex, sizeof(crcReceivedHex), "0x%04X", (unsigned)f.crc);
                            obj["crcReceivedHex"] = crcReceivedHex;

                            char crcCalculatedHex[7];
                            snprintf(crcCalculatedHex, sizeof(crcCalculatedHex), "0x%04X", (unsigned)calculatedCrc);
                            obj["crcCalculatedHex"] = crcCalculatedHex;

                            if (!f.isValid) {
                                obj["invalidReason"] = "crc_mismatch";
                                char why[64];
                                snprintf(why, sizeof(why), "CRC mismatch (received=%s calculated=%s)",
                                         crcReceivedHex, crcCalculatedHex);
                                obj["invalidWhy"] = why;
                            }
                        }
                        obj["hex"] = modbus.formatFrameHex(f);
                    };

                    if (best->hasBefore) {
                        JsonObject before = item["before"].to<JsonObject>();
                        writeFrame(before, best->before);
                    }

                    {
                        JsonObject bad = item["bad"].to<JsonObject>();
                        writeFrame(bad, best->bad);
                    }

                    if (best->hasAfter) {
                        JsonObject after = item["after"].to<JsonObject>();
                        writeFrame(after, best->after);
                    }
                }

                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            };

        webServer->on("/api/modbus/crc", HTTP_GET, handleModbusCrc);
        webServer->on("/api/modbus/crc/", HTTP_GET, handleModbusCrc);
        
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

        // Bus pattern analysis: per-register-range timing, gaps, cycle detection
        // GET /api/modbus/patterns         — full pattern report
        // POST /api/modbus/patterns/reset  — clear collected data
        webServer->on("/api/modbus/patterns", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                // Run cycle detection before responding
                modbus.detectCycle();

                // Pre-allocate 16KB buffer to avoid cbuf resize failures on ESP32
                AsyncResponseStream* response = request->beginResponseStream("application/json", 16384);

                const unsigned long nowMs = millis();

                response->print(F("{\"uptimeMs\":"));
                response->print((uint32_t)nowMs);

                // ---- Byte-level bus stats ----
                const BusByteStats& bs = modbus.getBusByteStats();
                {
                    uint32_t elapsedMs = (bs.lastUpdateMs > bs.startMs) ? (uint32_t)(bs.lastUpdateMs - bs.startMs) : 0;
                    float bytesPerSec = (elapsedMs > 0) ? ((float)bs.totalBytes * 1000.0f / (float)elapsedMs) : 0.0f;
                    response->printf(",\"byteStats\":{\"totalBytes\":%u,\"frameBoundaries\":%u,"
                                     "\"validFrames\":%u,\"invalidFrames\":%u,"
                                     "\"elapsedMs\":%u,\"bytesPerSec\":%.1f}",
                                     bs.totalBytes, bs.totalFrameBoundaries,
                                     bs.validFrames, bs.invalidFrames,
                                     elapsedMs, bytesPerSec);
                }

                // ---- Transaction time stats (request → response duration) ----
                {
                    const BusTransactionStats& t = modbus.getTransactionStats();
                    response->printf(",\"transactionTimes\":{\"count\":%u", t.count);
                    if (t.count > 0) {
                        double mean = t.sumMs / t.count;
                        double variance = (t.count > 1)
                            ? (t.sumSqMs - t.sumMs * t.sumMs / t.count) / (t.count - 1)
                            : 0.0;
                        double stddev = (variance > 0) ? sqrt(variance) : 0.0;
                        response->printf(",\"minMs\":%u,\"maxMs\":%u,\"meanMs\":%.1f,\"stddevMs\":%.1f",
                                         t.minMs, t.maxMs, mean, stddev);
                    }
                    static const char* txnLabels[] = {
                        "<10ms","10-20ms","20-50ms","50-100ms","100-200ms","200-500ms","500ms-1s",">=1s"
                    };
                    response->print(F(",\"histogram\":["));
                    for (size_t i = 0; i < BusTransactionStats::NUM_BUCKETS; ++i) {
                        if (i > 0) response->print(',');
                        response->printf("{\"label\":\"%s\",\"count\":%u}", txnLabels[i], t.buckets[i]);
                    }
                    response->print(F("]}"));
                }

                // ---- Per-register-range entries ----
                response->print(F(",\"entries\":["));
                bool first = true;
                for (const auto& kv : modbus.getBusPatterns()) {
                    const BusPatternEntry& e = kv.second;
                    if (!first) response->print(',');
                    first = false;
                    response->printf("{\"unitId\":%u,\"fc\":%u,\"startReg\":%u,\"qty\":%u,"
                                     "\"count\":%u,\"firstSeenMs\":%lu,\"lastSeenMs\":%lu",
                                     e.unitId, e.functionCode, e.startRegister, e.quantity,
                                     e.count, e.firstSeenMs, e.lastSeenMs);
                    if (e.intervalCount > 0) {
                        double mean = e.intervalSum / e.intervalCount;
                        double variance = (e.intervalCount > 1)
                            ? (e.intervalSumSq - e.intervalSum * e.intervalSum / e.intervalCount) / (e.intervalCount - 1)
                            : 0.0;
                        double stddev = (variance > 0) ? sqrt(variance) : 0.0;
                        response->printf(",\"interval\":{\"count\":%u,\"minMs\":%u,\"maxMs\":%u,"
                                         "\"meanMs\":%.1f,\"stddevMs\":%.1f}",
                                         e.intervalCount, e.intervalMin, e.intervalMax,
                                         mean, stddev);
                    }
                    // Successor gap: idle time after this transaction's response
                    if (e.successorGapCount > 0) {
                        double sgMean = e.successorGapSum / e.successorGapCount;
                        double sgVar = (e.successorGapCount > 1)
                            ? (e.successorGapSumSq - e.successorGapSum * e.successorGapSum / e.successorGapCount) / (e.successorGapCount - 1)
                            : 0.0;
                        double sgStd = (sgVar > 0) ? sqrt(sgVar) : 0.0;
                        response->printf(",\"successorGap\":{\"count\":%u,\"minMs\":%u,\"maxMs\":%u,"
                                         "\"meanMs\":%.1f,\"stddevMs\":%.1f}",
                                         e.successorGapCount, e.successorGapMin, e.successorGapMax,
                                         sgMean, sgStd);
                    }
                    response->print('}');
                }
                response->print(']');

                // ---- Inter-frame gap histogram (measured at byte level, in microseconds) ----
                const BusGapStats& g = modbus.getBusGapStats();
                response->print(F(",\"gaps\":{"));
                response->printf("\"count\":%u", g.count);
                if (g.count > 0) {
                    double meanUs = g.sumUs / g.count;
                    double variance = (g.count > 1)
                        ? (g.sumSqUs - g.sumUs * g.sumUs / g.count) / (g.count - 1)
                        : 0.0;
                    double stddevUs = (variance > 0) ? sqrt(variance) : 0.0;
                    response->printf(",\"minUs\":%u,\"maxUs\":%u,\"meanUs\":%.0f,\"stddevUs\":%.0f,"
                                     "\"minMs\":%.2f,\"maxMs\":%.2f,\"meanMs\":%.2f",
                                     g.minUs, g.maxUs, meanUs, stddevUs,
                                     (float)g.minUs / 1000.0f, (float)g.maxUs / 1000.0f,
                                     (float)meanUs / 1000.0f);
                }
                // Buckets with readable labels
                response->print(F(",\"histogram\":["));
                static const char* gapLabels[] = {
                    "<1ms","1-3ms","3-5ms","5-10ms","10-20ms","20-50ms",
                    "50-100ms","100-200ms","200-500ms","500ms-1s","1-5s",">=5s"
                };
                for (size_t i = 0; i < BusGapStats::NUM_BUCKETS; ++i) {
                    if (i > 0) response->print(',');
                    response->printf("{\"label\":\"%s\",\"count\":%u}", gapLabels[i], g.buckets[i]);
                }
                response->print(F("]}"));

                // ---- Detected cycle with per-step gap stats ----
                const auto& cycle = modbus.getDetectedCycle();
                const auto& stepGaps = modbus.getCycleStepGaps();
                response->printf(",\"cyclePosition\":%d", modbus.getCycleTrackingPos());
                response->print(F(",\"cycle\":["));
                for (size_t i = 0; i < cycle.size(); ++i) {
                    if (i > 0) response->print(',');
                    const BusCycleEntry& c = cycle[i];
                    response->printf("{\"unitId\":%u,\"fc\":%u,\"startReg\":%u,\"qty\":%u",
                                     c.unitId, c.functionCode, c.startRegister, c.quantity);
                    if (i < stepGaps.size() && stepGaps[i].count > 0) {
                        const CycleStepStats& sg = stepGaps[i];
                        double sgMean = sg.sumMs / sg.count;
                        double sgVar = (sg.count > 1)
                            ? (sg.sumSqMs - sg.sumMs * sg.sumMs / sg.count) / (sg.count - 1)
                            : 0.0;
                        double sgStddev = (sgVar > 0) ? sqrt(sgVar) : 0.0;
                        response->printf(",\"gap\":{\"count\":%u,\"minMs\":%u,\"maxMs\":%u,"
                                         "\"meanMs\":%.1f,\"stddevMs\":%.1f}",
                                         sg.count, sg.minMs, sg.maxMs, sgMean, sgStddev);
                    }
                    response->print('}');
                }
                response->print(F("]"));

                // ---- Transitions: which request follows which? ----
                {
                    const BusTransitionMap& transitions = modbus.getBusTransitions();
                    const auto& patterns = modbus.getBusPatterns();
                    response->print(F(",\"transitions\":["));
                    bool firstTx = true;
                    for (const auto& pred : transitions) {
                        // Find predecessor pattern info
                        auto predPat = patterns.find(pred.first);
                        if (predPat == patterns.end()) continue;
                        const BusPatternEntry& pe = predPat->second;

                        // Find top successors by count (limit to top 4 to save bandwidth)
                        struct SuccEntry { uint64_t key; const BusTransitionEntry* te; };
                        SuccEntry top[4] = {};
                        size_t topN = 0;
                        for (const auto& succ : pred.second) {
                            if (topN < 4) {
                                top[topN++] = {succ.first, &succ.second};
                            } else {
                                // Replace smallest
                                size_t minIdx = 0;
                                for (size_t j = 1; j < 4; j++) {
                                    if (top[j].te->count < top[minIdx].te->count) minIdx = j;
                                }
                                if (succ.second.count > top[minIdx].te->count) {
                                    top[minIdx] = {succ.first, &succ.second};
                                }
                            }
                        }

                        if (!firstTx) response->print(',');
                        firstTx = false;
                        response->printf("{\"from\":{\"unit\":%u,\"fc\":%u,\"reg\":%u,\"qty\":%u},\"to\":[",
                                         pe.unitId, pe.functionCode, pe.startRegister, pe.quantity);
                        bool firstSucc = true;
                        for (size_t i = 0; i < topN; i++) {
                            auto succPat = patterns.find(top[i].key);
                            if (succPat == patterns.end()) continue;
                            const BusPatternEntry& se = succPat->second;
                            const BusTransitionEntry& te = *top[i].te;
                            if (!firstSucc) response->print(',');
                            firstSucc = false;
                            response->printf("{\"unit\":%u,\"fc\":%u,\"reg\":%u,\"qty\":%u,\"count\":%u",
                                             se.unitId, se.functionCode, se.startRegister, se.quantity, te.count);
                            if (te.count > 0) {
                                double gMean = te.gapSum / te.count;
                                double gVar = (te.count > 1)
                                    ? (te.gapSumSq - te.gapSum * te.gapSum / te.count) / (te.count - 1)
                                    : 0.0;
                                double gStd = (gVar > 0) ? sqrt(gVar) : 0.0;
                                response->printf(",\"gap\":{\"minMs\":%u,\"maxMs\":%u,\"meanMs\":%.1f,\"stddevMs\":%.1f}",
                                                 te.gapMin, te.gapMax, gMean, gStd);
                            }
                            response->print('}');
                        }
                        response->print("]}");
                    }
                    response->print(']');
                }

                response->print('}');
                request->send(response);
            });

        webServer->on("/api/modbus/patterns/reset", HTTP_POST,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();
                modbus.resetBusPatterns();
                request->send(200, "application/json", "{\"reset\":true}");
            });

        // ---- Gap Scheduler API ----
        webServer->on("/api/modbus/gap-scheduler", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                const auto& gs = modbus.getGapSchedulerStats();
                auto gap = modbus.predictCurrentGap();
                uint32_t totalTx = gs.txInGap + gs.txFallback;
                float gapPct = totalTx > 0 ? (gs.txInGap * 100.0f / totalTx) : 0;
                float collisionRate = gs.txInGap > 0 ? (gs.collisions * 100.0f / gs.txInGap) : 0;
                float successRate = (gs.gapSufficient + gs.gapInsufficient) > 0
                    ? (gs.gapSufficient * 100.0f / (gs.gapSufficient + gs.gapInsufficient)) : 0;

                auto* response = request->beginResponseStream("application/json");
                response->print(F("{\"txDecisions\":{"));
                response->printf("\"inGap\":%u,\"fallback\":%u,\"deferred\":%u,\"total\":%u,\"gapPct\":%.1f",
                    gs.txInGap, gs.txFallback, gs.txDeferred, totalTx, gapPct);
                response->print(F("},\"prediction\":{"));
                response->printf("\"used\":%u,\"sufficient\":%u,\"insufficient\":%u,\"skippedSmall\":%u,\"successRate\":%.1f",
                    gs.predictionsUsed, gs.gapSufficient, gs.gapInsufficient, gs.gapSkippedSmall, successRate);
                response->print(F("},\"collisions\":{"));
                response->printf("\"count\":%u,\"rate\":%.2f", gs.collisions, collisionRate);
                if (gs.lastCollisionMs > 0) {
                    response->printf(",\"lastAgoMs\":%lu", (unsigned long)(millis() - gs.lastCollisionMs));
                }
                response->print(F("},\"margin\":{"));
                response->printf("\"current\":%.2f,\"initial\":%.2f,\"min\":%.2f,\"max\":%.2f",
                    gs.safetyMargin, gs.initialMargin, gs.minMargin, gs.maxMargin);
                if (gs.lastMarginAdjustMs > 0) {
                    response->printf(",\"lastAdjustAgoMs\":%lu", (unsigned long)(millis() - gs.lastMarginAdjustMs));
                }
                response->print(F("},\"wireTime\":{"));
                response->printf("\"totalMs\":%u", gs.totalWireTimeMs);
                response->print(F("},\"registers\":{"));
                response->printf("\"total\":%u", gs.registersRead);
                uint32_t elapsedSec = (millis() - gs.startMs) / 1000;
                float regsPerSec = elapsedSec > 0 ? (float)gs.registersRead / elapsedSec : 0;
                response->printf(",\"perSecond\":%.2f,\"elapsedSec\":%u", regsPerSec, elapsedSec);
                response->print(F("},\"currentGap\":{"));
                response->printf("\"valid\":%s", gap.valid ? "true" : "false");
                if (gap.valid) {
                    response->printf(",\"predictedMs\":%u,\"confirmationMs\":%u,\"minObservedMs\":%u,\"samples\":%u",
                        gap.predictedGapMs, gap.confirmationMs, gap.minObservedMs, gap.sampleCount);
                }
                uint32_t gmin = modbus.getGlobalMinGapMs();
                response->printf("},\"globalMinGapMs\":%u", gmin != UINT32_MAX ? gmin : 0);
                response->print(F(",\"timing\":{"));
                if (gs.lastTxMs > 0) {
                    response->printf("\"lastTxAgoMs\":%lu", (unsigned long)(millis() - gs.lastTxMs));
                }
                response->print(F("}")); // timing
                response->print('}');
                request->send(response);
            });

        // ---- Human-friendly bus pattern analysis page ----
        static const char PATTERNS_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bus Pattern Analysis</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,system-ui,sans-serif;padding:16px;max-width:1200px;margin:0 auto}
h1{color:#4fc3f7;margin-bottom:16px;font-size:1.5em}
h2{color:#81c784;border-bottom:1px solid #333;padding-bottom:4px;margin:24px 0 10px;font-size:1.15em}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:16px}
.card{background:#16213e;border-radius:8px;padding:12px;text-align:center}
.card .v{font-size:1.4em;color:#4fc3f7;word-break:break-all}
.card .l{font-size:.7em;color:#888;margin-top:2px}
table{border-collapse:collapse;width:100%;margin-bottom:16px;font-size:.85em}
th,td{border:1px solid #2a2a4a;padding:4px 8px;text-align:right}
th{background:#16213e;color:#4fc3f7;position:sticky;top:0}
td:first-child,th:first-child{text-align:left}
tr:nth-child(even){background:rgba(255,255,255,.02)}
.histo{margin:8px 0 16px}
.bar-row{display:flex;align-items:center;gap:6px;margin:2px 0}
.bar{background:linear-gradient(90deg,#4fc3f7,#0288d1);height:18px;border-radius:2px;min-width:1px;transition:width .3s}
.bar-label{min-width:72px;font-size:.8em;text-align:right;color:#aaa}
.bar-count{font-size:.8em;min-width:36px}
.bar-pct{font-size:.72em;color:#666;min-width:40px}
.controls{display:flex;gap:10px;margin-bottom:16px;align-items:center;flex-wrap:wrap}
button{background:#4fc3f7;color:#000;border:none;padding:6px 14px;cursor:pointer;border-radius:4px;font-size:.85em;font-weight:600}
button:hover{background:#81d4fa}
button.danger{background:#ef5350;color:#fff}
button.danger:hover{background:#e53935}
.status{font-size:.8em;color:#666;margin-left:auto}
.note{background:#16213e;border-left:3px solid #4fc3f7;padding:10px 14px;margin:10px 0;font-size:.85em;border-radius:0 6px 6px 0}
.cycle-step{display:flex;align-items:center;gap:8px;padding:5px 8px;border-bottom:1px solid #1a1a2e}
.cycle-step:nth-child(even){background:rgba(255,255,255,.02)}
.cycle-step.current{background:#1a3a2a;border-left:3px solid #81c784}
.cycle-num{min-width:28px;color:#666;font-size:.8em;text-align:right}
.cycle-desc{flex:1;font-size:.9em}
.cycle-gap{min-width:90px;text-align:right;font-size:.85em;font-weight:500}
.gap-ok{color:#81c784}.gap-tight{color:#ffb74d}.gap-no{color:#ef5350}
a{color:#4fc3f7}
.footer{margin-top:24px;font-size:.75em;color:#555;border-top:1px solid #222;padding-top:10px}
</style></head><body>
<h1>&#128268; Bus Pattern Analysis</h1>
<div class="controls">
<button onclick="refresh()">&#8635; Refresh</button>
<button class="danger" onclick="resetData()">&#9249; Reset</button>
<label style="font-size:.85em"><input type="checkbox" id="autoRef" checked onchange="toggleAuto()"> Auto (10s)</label>
<span class="status" id="status">Loading...</span>
</div>
<div id="content"></div>
<script>
let T=null;
const $=s=>document.getElementById(s);
function toggleAuto(){if($('autoRef').checked)startA();else stopA();}
function startA(){stopA();T=setInterval(refresh,10000);}
function stopA(){if(T){clearInterval(T);T=null;}}

async function api(p,m='GET'){
  const r=await fetch(p,{method:m,credentials:'include'});
  if(!r.ok)throw new Error(r.status);return r.json();}

async function refresh(){
  $('status').textContent='Fetching...';
  try{const d=await api('/api/modbus/patterns');render(d);
  $('status').textContent='Updated '+new Date().toLocaleTimeString();
  }catch(e){$('status').textContent='Error: '+e.message;}}

async function resetData(){
  if(!confirm('Reset all collected pattern data?'))return;
  await api('/api/modbus/patterns/reset','POST');refresh();}

function fmt(n,d){return typeof n==='number'?n.toFixed(d===undefined?1:d):String(n);}
function fms(ms){if(ms==null)return'-';if(ms<1000)return fmt(ms,0)+'ms';return fmt(ms/1000,1)+'s';}

function histo(buckets){
  if(!buckets||!buckets.length)return'';
  const mx=Math.max(...buckets.map(b=>b.count),1);
  const tot=buckets.reduce((s,b)=>s+b.count,0)||1;
  return'<div class="histo">'+buckets.map(b=>{
    const w=Math.max(b.count/mx*100,0.3);
    const p=(b.count/tot*100).toFixed(1);
    return`<div class="bar-row"><span class="bar-label">${b.label}</span>`+
      `<div class="bar" style="width:${w}%"></div>`+
      `<span class="bar-count">${b.count}</span>`+
      `<span class="bar-pct">${b.count?p+'%':''}</span></div>`;
  }).join('')+'</div>';
}

function render(d){
  const bs=d.byteStats||{};
  const vf=bs.validFrames||0,iv=bs.invalidFrames||0;
  const vpct=(vf+iv)>0?(vf/(vf+iv)*100).toFixed(1):'?';
  let h=`<div class="cards">
    <div class="card"><div class="v">${fms(d.uptimeMs)}</div><div class="l">Uptime</div></div>
    <div class="card"><div class="v">${bs.totalBytes||0}</div><div class="l">Total Bytes</div></div>
    <div class="card"><div class="v">${vpct}%</div><div class="l">Valid Frames</div></div>
    <div class="card"><div class="v">${fmt(bs.bytesPerSec||0,1)}</div><div class="l">Bytes/sec</div></div>
    <div class="card"><div class="v">${vf} / ${iv}</div><div class="l">Valid / Invalid</div></div>
    <div class="card"><div class="v">${bs.frameBoundaries||0}</div><div class="l">Frame Boundaries</div></div>
  </div>`;

  // Transaction times
  const t=d.transactionTimes;
  h+=`<h2>&#9201; Transaction Times (Request &#8594; Response)</h2>`;
  if(t&&t.count>0){
    h+=`<div class="cards">
      <div class="card"><div class="v">${t.count}</div><div class="l">Transactions</div></div>
      <div class="card"><div class="v">${fms(t.minMs)}</div><div class="l">Min RTT</div></div>
      <div class="card"><div class="v">${fms(t.meanMs)}</div><div class="l">Mean RTT</div></div>
      <div class="card"><div class="v">${fms(t.maxMs)}</div><div class="l">Max RTT</div></div>
    </div>`+histo(t.histogram);
    const needed=Math.ceil(t.meanMs*1.5);
    h+=`<div class="note">&#128161; To fit one own request you need at least <b>~${fms(needed)}</b> gap (mean RTT + 50% margin).</div>`;
  }else h+=`<p style="color:#666">No paired request&#8594;response transactions observed yet.</p>`;

  // Polling register map
  h+=`<h2>&#128203; Polling Register Map</h2>`;
  if(d.entries&&d.entries.length>0){
    h+=`<table><tr><th>Unit</th><th>FC</th><th>Start Reg</th><th>Qty</th><th>Count</th><th>Interval</th><th>&#963;</th></tr>`;
    for(const e of d.entries){
      const iv=e.interval;
      h+=`<tr><td>${e.unitId}</td><td>FC${e.fc}</td><td>${e.startReg}</td><td>${e.qty}</td><td>${e.count}</td>`+
        `<td>${iv?fms(iv.meanMs):'-'}</td><td>${iv?'\u00b1'+fms(iv.stddevMs):''}</td></tr>`;
    }
    h+=`</table>`;
  }else h+=`<p style="color:#666">No register patterns detected yet.</p>`;

  // Inter-frame gaps
  h+=`<h2>&#8596; Inter-Frame Gaps</h2>`;
  const g=d.gaps;
  if(g&&g.count>0){
    h+=`<div class="cards">
      <div class="card"><div class="v">${g.count}</div><div class="l">Gaps</div></div>
      <div class="card"><div class="v">${fms(g.minMs)}</div><div class="l">Min</div></div>
      <div class="card"><div class="v">${fms(g.meanMs)}</div><div class="l">Mean</div></div>
      <div class="card"><div class="v">${fms(g.maxMs)}</div><div class="l">Max</div></div>
    </div>`+histo(g.histogram);
    if(t&&t.count>0){
      const need=t.meanMs*1.5;const needUs=need*1000;
      const bounds=[1000,3000,5000,10000,20000,50000,100000,200000,500000,1000000,5000000,1e15];
      let ok=0;for(let i=0;i<g.histogram.length;i++){
        const lo=i>0?bounds[i-1]:0;if(lo>=needUs)ok+=g.histogram[i].count;}
      const pct=(ok/g.count*100).toFixed(1);
      h+=`<div class="note">&#128640; ${pct}% of gaps (${ok}/${g.count}) are large enough for one request (~${fms(need)}).</div>`;
    }
  }else h+=`<p style="color:#666">No gap data yet.</p>`;

  // Detected cycle
  h+=`<h2>&#128260; Detected Polling Cycle</h2>`;
  const cy=d.cycle;
  if(cy&&cy.length>0){
    const pos=d.cyclePosition!=null?d.cyclePosition:-1;
    const need=t&&t.count>0?t.meanMs*1.5:50;
    h+=`<div style="background:#16213e;border-radius:8px;padding:8px;margin-bottom:10px">`;
    for(let i=0;i<cy.length;i++){
      const c=cy[i];const cur=i===pos;
      let gc='',gt='';
      if(c.gap&&c.gap.count>0){
        const mg=c.gap.meanMs;
        if(mg>=need*2){gc='gap-ok';gt=fms(mg)+' \u2714';}
        else if(mg>=need){gc='gap-tight';gt=fms(mg)+' \u2248';}
        else{gc='gap-no';gt=fms(mg)+' \u2718';}
        gt+=` (\u00b1${fms(c.gap.stddevMs)})`;
      }
      h+=`<div class="cycle-step${cur?' current':''}">
        <span class="cycle-num">${i+1}.</span>
        <span class="cycle-desc">Unit ${c.unitId} FC${c.fc} reg ${c.startReg}\u00d7${c.qty}</span>
        <span class="cycle-gap ${gc}">${gt}</span></div>`;
    }
    h+=`</div>`;
    if(pos>=0)h+=`<div class="note">Currently tracking at step <b>${pos+1}</b> of ${cy.length}. Gap colors: <span class="gap-ok">\u2714 safe</span> / <span class="gap-tight">\u2248 tight</span> / <span class="gap-no">\u2718 too short</span></div>`;
    else h+=`<div class="note">Cycle detected (${cy.length} steps). Gap tracking will sync on next matching request.</div>`;
  }else h+=`<p style="color:#666">No repeating cycle detected yet. Collect more data (2+ minutes).</p>`;

  h+=`<div class="footer"><a href="/">\u2190 Home</a> &middot; <a href="/view/modbus/scheduler">Gap Scheduler</a> &middot; <a href="/api/modbus/patterns">Raw JSON</a></div>`;
  $('content').innerHTML=h;
}

refresh();startA();
</script></body></html>)rawliteral";

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
        
        // Response mismatch diagnostics
        webServer->on("/api/modbus/mismatches", HTTP_GET,
            [&modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                size_t count;
                uint32_t totalMismatches;
                const auto* history = modbus.getMismatchHistory(count, totalMismatches);

                JsonDocument doc;
                doc["totalMismatches"] = totalMismatches;
                doc["uptimeMs"] = millis();
                
                JsonArray arr = doc["history"].to<JsonArray>();
                for (size_t i = 0; i < count; i++) {
                    const auto& m = history[i];
                    if (m.timestamp == 0) continue;
                    
                    JsonObject obj = arr.add<JsonObject>();
                    obj["timestampMs"] = m.timestamp;
                    obj["expectedUnit"] = m.expectedUnit;
                    obj["actualUnit"] = m.actualUnit;
                    obj["expectedFc"] = m.expectedFc;
                    obj["actualFc"] = m.actualFc;
                    obj["byteCountMatch"] = m.byteCountMatch;
                    
                    // Diagnose the issue
                    if (m.expectedUnit != m.actualUnit) {
                        obj["issue"] = "unit_mismatch";
                    } else if (m.expectedFc != m.actualFc) {
                        obj["issue"] = "fc_mismatch";
                    } else if (!m.byteCountMatch) {
                        obj["issue"] = "bytecount_mismatch";
                    } else {
                        obj["issue"] = "unknown";
                    }
                }
                
                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });
        
        // HTML dashboard
        // JSON endpoint for AJAX updates
        webServer->on("/api/modbus/dashboard", HTTP_GET,
            [&devices, &modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                AsyncResponseStream* response = request->beginResponseStream("application/json");
                response->print(F("{\"status\":{"));
                response->printf("\"silent\":%s,", modbus.isBusSilent() ? "true" : "false");
                response->printf("\"queue\":%u,", modbus.getPendingRequestCount());
                response->printf("\"rx\":%lu,", modbus.getStats().framesReceived);
                response->printf("\"tx\":%lu,", modbus.getStats().framesSent);
                response->printf("\"crcErrors\":%lu", modbus.getStats().crcErrors);
                response->print(F("},\"devices\":["));

                bool firstDev = true;
                unsigned long now = millis();
                for (const auto& kv : devices.getDevices()) {
                    const auto& dev = kv.second;
                    if (!firstDev) response->print(',');
                    firstDev = false;

                    response->printf("{\"unitId\":%u,\"type\":\"%s\",\"success\":%lu,\"errors\":%lu,\"values\":[",
                        dev.unitId, dev.deviceTypeName.c_str(), dev.successCount, dev.errorCount);

                    bool firstVal = true;
                    for (const auto& val : dev.currentValues) {
                        uint32_t pollIntervalMs = 0;
                        if (dev.deviceType) {
                            for (const auto& reg : dev.deviceType->registers) {
                                if (strcmp(reg.name, val.second.name) == 0) {
                                    pollIntervalMs = (uint32_t)(reg.pollIntervalMs * dev.pollIntervalFactor);
                                    break;
                                }
                            }
                        }
                        bool isOutdated = pollIntervalMs > 0 && (now - val.second.updatedAtMs) > (pollIntervalMs * 3);

                        if (!firstVal) response->print(',');
                        firstVal = false;
                        response->printf("{\"n\":\"%s\",\"v\":%.2f,\"u\":\"%s\",\"ok\":%s,\"old\":%s}",
                            val.second.name, val.second.value, val.second.unit,
                            val.second.valid ? "true" : "false",
                            isOutdated ? "true" : "false");
                    }
                    response->print(F("]}"));
                }
                response->print(F("]}")); 
                request->send(response);
            });

        // HTML pages: register more-specific paths first to avoid
        // ESPAsyncWebServer prefix-match collisions with /view/modbus.

        // ---- Gap Scheduler monitoring page ----
        static const char SCHEDULER_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gap Scheduler</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1a1a2e;color:#e0e0e0;font-family:-apple-system,system-ui,sans-serif;padding:16px;max-width:900px;margin:0 auto}
h1{color:#4fc3f7;margin-bottom:16px;font-size:1.5em}
h2{color:#81c784;border-bottom:1px solid #333;padding-bottom:4px;margin:20px 0 10px;font-size:1.15em}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px;margin-bottom:16px}
.card{background:#16213e;border-radius:8px;padding:12px;text-align:center}
.card .v{font-size:1.4em;color:#4fc3f7;word-break:break-all}
.card .l{font-size:.7em;color:#888;margin-top:2px}
.ok .v{color:#81c784} .warn .v{color:#ffb74d} .bad .v{color:#ef5350}
.bar-bg{background:#0d1117;border-radius:4px;height:22px;overflow:hidden;margin:6px 0}
.bar-fill{height:100%;border-radius:4px;transition:width .5s}
.bar-green{background:linear-gradient(90deg,#43a047,#81c784)}
.bar-orange{background:linear-gradient(90deg,#f57c00,#ffb74d)}
.bar-red{background:linear-gradient(90deg,#c62828,#ef5350)}
.bar-blue{background:linear-gradient(90deg,#0288d1,#4fc3f7)}
table{border-collapse:collapse;width:100%;font-size:.85em;margin-bottom:16px}
th,td{border:1px solid #2a2a4a;padding:4px 8px;text-align:right}
th{background:#16213e;color:#4fc3f7;position:sticky;top:0}
td:first-child,th:first-child{text-align:left}
.controls{display:flex;gap:10px;margin-bottom:16px;align-items:center;flex-wrap:wrap}
button{background:#4fc3f7;color:#000;border:none;padding:6px 14px;cursor:pointer;border-radius:4px;font-size:.85em;font-weight:600}
button:hover{background:#81d4fa}
.status{font-size:.8em;color:#666;margin-left:auto}
a{color:#4fc3f7}
.footer{margin-top:24px;font-size:.75em;color:#555;border-top:1px solid #222;padding-top:10px}
</style></head><body>
<h1>&#9201; Gap Scheduler Monitor</h1>
<div class="controls">
<button onclick="refresh()">&#8635; Refresh</button>
<label style="font-size:.85em"><input type="checkbox" id="autoRef" checked onchange="toggleAuto()"> Auto (5s)</label>
<span class="status" id="status">Loading...</span>
</div>
<div id="content"></div>
<div class="footer"><a href="/view/modbus">&larr; Dashboard</a> | <a href="/view/modbus/patterns">Patterns</a></div>
<script>
let T=null;
const $=s=>document.getElementById(s);
function toggleAuto(){if($('autoRef').checked)startA();else stopA();}
function startA(){stopA();T=setInterval(refresh,5000);}
function stopA(){if(T){clearInterval(T);T=null;}}

async function refresh(){
  $('status').textContent='Fetching...';
  try{
    const d=await(await fetch('/api/modbus/gap-scheduler',{credentials:'include'})).json();
    render(d);
    $('status').textContent='Updated '+new Date().toLocaleTimeString();
  }catch(e){$('status').textContent='Error: '+e.message;}}

function fmt(n,d){return typeof n==='number'?n.toFixed(d===undefined?1:d):String(n);}
function fms(ms){if(ms==null||ms===undefined)return'-';if(ms<1000)return fmt(ms,0)+'ms';if(ms<60000)return fmt(ms/1000,1)+'s';return fmt(ms/60000,1)+'m';}

function pctBar(pct,cls){
  return '<div class="bar-bg"><div class="bar-fill '+cls+'" style="width:'+Math.min(pct,100)+'%"></div></div>';
}

function cardClass(val,warnThresh,badThresh,invert){
  if(invert){if(val>=warnThresh)return'ok';if(val>=badThresh)return'warn';return'bad';}
  if(val<=warnThresh)return'ok';if(val<=badThresh)return'warn';return'bad';
}

function render(d){
  const tx=d.txDecisions, pr=d.prediction, co=d.collisions, mg=d.margin, cg=d.currentGap;
  let h='';

  // TX Decision summary
  h+='<h2>TX Decisions</h2><div class="cards">';
  h+='<div class="card ok"><div class="v">'+tx.inGap+'</div><div class="l">In Gap</div></div>';
  h+='<div class="card"><div class="v">'+tx.fallback+'</div><div class="l">Fallback</div></div>';
  h+='<div class="card"><div class="v">'+tx.deferred+'</div><div class="l">Deferred</div></div>';
  h+='<div class="card"><div class="v">'+tx.total+'</div><div class="l">Total TX</div></div>';
  h+='</div>';

  // Gap utilization bar
  h+='<div style="font-size:.85em;margin-bottom:4px">Gap Utilization: <b>'+fmt(tx.gapPct,1)+'%</b> of TX used gap prediction</div>';
  h+=pctBar(tx.gapPct,'bar-green');

  // Prediction quality
  h+='<h2>Prediction Quality</h2><div class="cards">';
  let sr=pr.successRate;
  let srCls=cardClass(sr,95,80,true);
  h+='<div class="card '+srCls+'"><div class="v">'+fmt(sr,1)+'%</div><div class="l">Success Rate</div></div>';
  h+='<div class="card ok"><div class="v">'+pr.sufficient+'</div><div class="l">Sufficient</div></div>';
  h+='<div class="card '+(pr.insufficient>0?'bad':'ok')+'"><div class="v">'+pr.insufficient+'</div><div class="l">Insufficient</div></div>';
  h+='<div class="card"><div class="v">'+pr.skippedSmall+'</div><div class="l">Skipped (small)</div></div>';
  h+='</div>';
  h+='<div style="font-size:.85em;margin-bottom:4px">Prediction Success</div>';
  h+=pctBar(sr,'bar-blue');

  // Collisions
  h+='<h2>Collisions</h2><div class="cards">';
  let coCls=cardClass(co.count,0,3,false);
  h+='<div class="card '+coCls+'"><div class="v">'+co.count+'</div><div class="l">Total</div></div>';
  h+='<div class="card '+cardClass(co.rate,1,5,false)+'"><div class="v">'+fmt(co.rate,2)+'%</div><div class="l">Rate</div></div>';
  h+='<div class="card"><div class="v">'+(co.lastAgoMs!=null?fms(co.lastAgoMs):'-')+'</div><div class="l">Last Ago</div></div>';
  h+='</div>';

  // Safety Margin
  h+='<h2>Safety Margin</h2><div class="cards">';
  let mgPct=mg.current*100;
  h+='<div class="card"><div class="v">'+fmt(mgPct,0)+'%</div><div class="l">Current</div></div>';
  h+='<div class="card"><div class="v">'+fmt(mg.initial*100,0)+'%</div><div class="l">Initial</div></div>';
  h+='<div class="card"><div class="v">'+fmt(mg.min*100,0)+'-'+fmt(mg.max*100,0)+'%</div><div class="l">Range</div></div>';
  if(mg.lastAdjustAgoMs!=null)
    h+='<div class="card"><div class="v">'+fms(mg.lastAdjustAgoMs)+'</div><div class="l">Last Adjust</div></div>';
  h+='</div>';
  // margin bar: show position within min..max range
  let mgRange=mg.max-mg.min;
  let mgPos=mgRange>0?((mg.current-mg.min)/mgRange*100):50;
  h+='<div style="font-size:.85em;margin-bottom:4px">Margin position ('+fmt(mg.min*100,0)+'% .. '+fmt(mg.max*100,0)+'%)</div>';
  h+=pctBar(mgPos, mgPct>40?'bar-red':mgPct>25?'bar-orange':'bar-green');

  // Current Gap Prediction
  h+='<h2>Current Gap Prediction</h2><div class="cards">';
  if(cg.valid){
    h+='<div class="card ok"><div class="v">'+cg.predictedMs+'ms</div><div class="l">Predicted</div></div>';
    h+='<div class="card"><div class="v">'+cg.confirmationMs+'ms</div><div class="l">Confirmation Wait</div></div>';
    h+='<div class="card"><div class="v">'+cg.minObservedMs+'ms</div><div class="l">Min Observed</div></div>';
    h+='<div class="card"><div class="v">'+cg.samples+'</div><div class="l">Samples</div></div>';
  } else {
    h+='<div class="card warn"><div class="v">N/A</div><div class="l">No valid prediction</div></div>';
  }
  h+='</div>';

  // Wire time & timing
  h+='<h2>Timing</h2><div class="cards">';
  h+='<div class="card"><div class="v">'+fms(d.wireTime.totalMs)+'</div><div class="l">Total Wire Time</div></div>';
  if(d.timing.lastTxAgoMs!=null)
    h+='<div class="card"><div class="v">'+fms(d.timing.lastTxAgoMs)+'</div><div class="l">Last TX Ago</div></div>';
  if(d.registers){
    let rps=d.registers.perSecond;
    let rpsCls=rps>=1?'ok':rps>0?'warn':'bad';
    h+='<div class="card '+rpsCls+'"><div class="v">'+fmt(rps,2)+'</div><div class="l">Registers/sec</div></div>';
    h+='<div class="card"><div class="v">'+d.registers.total+'</div><div class="l">Registers Read</div></div>';
  }
  h+='</div>';

  $('content').innerHTML=h;
}

refresh();
if($('autoRef').checked)startA();
</script>
</body></html>)rawliteral";

        webServer->on("/view/modbus/scheduler", HTTP_GET,
            [&server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();
                request->send(200, "text/html", SCHEDULER_PAGE);
            });

        webServer->on("/view/modbus/patterns", HTTP_GET,
            [&server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();
                request->send(200, "text/html", PATTERNS_PAGE);
            });

        webServer->on("/view/modbus/raw", HTTP_GET,
            [&server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                String html = F("<!DOCTYPE html><html><head>"
                    "<title>Modbus Raw Tools</title>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<style>"
                    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial;margin:10px;background:#1a1a2e;color:#eee}"
                    ".card{background:#16213e;border-radius:8px;padding:10px;margin:10px 0;box-shadow:0 2px 4px rgba(0,0,0,0.3);width:max-content;min-width:100%}"
                    "h1{color:#FF9800}h2{color:#eee;margin:0 0 10px 0}h3{color:#ccc;margin:10px 0 5px 0}"
                    "a{color:#FF9800}"
                    "label{display:inline-block;margin:6px 10px 6px 0}"
                    "input,select{padding:6px;background:#0f3460;color:#eee;border:1px solid #2a2a4a;border-radius:4px}"
                    "button{padding:6px 12px;background:#FF9800;color:#1a1a2e;border:none;border-radius:4px;font-weight:600;cursor:pointer}"
                    "button:hover{background:#F57C00}"
                    "pre{background:#0a0a1a;color:#eee;padding:10px;border-radius:6px;overflow:auto;border:1px solid #2a2a4a}"
                    "code{background:#0f3460;padding:2px 6px;border-radius:3px}"
                    "small{color:#888}"
                    "</style></head><body>"
                    "<h1>Modbus Raw Tools</h1>"
                    "<p><a href='/view/modbus'>&larr; Back to dashboard</a></p>"
                    "<div class='card'>"
                    "<h2>Tracked Raw Read</h2>"
                    "<p><small>Sends via <code>/api/modbus/raw/readTracked</code> and polls <code>/api/modbus/raw/result</code>.</small></p>"
                    "<div>"
                    "<label>unit <input id='unit' type='number' value='1' min='1' max='247'></label>"
                    "<label>address <input id='address' type='number' value='0' min='0' max='65535'></label>"
                    "<label>count <input id='count' type='number' value='2' min='1' max='125'></label>"
                    "<label>fc <select id='fc'><option value='3'>3</option><option value='4'>4</option></select></label>"
                    "<button onclick='sendRead()'>Send</button>"
                    "</div>"
                    "<h3>Request Frame (hex)</h3><pre id='req'>-</pre>"
                    "<h3>Result</h3><pre id='out'>Ready.</pre>"
                    "</div>"
                    "<script>"
                    "let lastRequestId = 0;"
                    "function qs(id){return document.getElementById(id);}"
                    "function toHexByte(b){return ('0'+(b&0xFF).toString(16)).slice(-2).toUpperCase();}"
                    "function toHex(bytes){return bytes.map(toHexByte).join(' ');}"
                    "function crc16Modbus(bytes){"
                    "  let crc=0xFFFF;"
                    "  for(const bb of bytes){"
                    "    crc ^= (bb & 0xFF);"
                    "    for(let i=0;i<8;i++){"
                    "      const lsb = crc & 1;"
                    "      crc >>= 1;"
                    "      if(lsb) crc ^= 0xA001;"
                    "    }"
                    "  }"
                    "  return crc & 0xFFFF;"
                    "}"
                    "async function sendRead(){"
                    "  const u=qs('unit').value, a=qs('address').value, c=qs('count').value, fc=qs('fc').value;"
                    "  const unit = parseInt(u,10)||0;"
                    "  const addr = parseInt(a,10)||0;"
                    "  const cnt  = parseInt(c,10)||0;"
                    "  const fcc  = parseInt(fc,10)||3;"
                    "  const req = [unit, fcc, (addr>>8)&0xFF, addr&0xFF, (cnt>>8)&0xFF, cnt&0xFF];"
                    "  const crc = crc16Modbus(req);"
                    "  req.push(crc & 0xFF, (crc>>8)&0xFF);"
                    "  qs('req').textContent = toHex(req);"
                    "  qs('out').textContent='Queueing...';"
                    "  const url=`/api/modbus/raw/readTracked?unit=${encodeURIComponent(u)}&address=${encodeURIComponent(a)}&count=${encodeURIComponent(c)}&fc=${encodeURIComponent(fc)}`;"
                    "  const r=await fetch(url);"
                    "  const j=await r.json();"
                    "  lastRequestId = j.requestId || 0;"
                    "  qs('out').textContent = JSON.stringify(j,null,2);"
                    "  if(!j.queued || !lastRequestId) return;"
                    "  pollResult(lastRequestId, 0);"
                    "}"
                    "async function pollResult(id, n){"
                    "  if(n>40){ qs('out').textContent += `\n\nNo response yet (timeout waiting in UI).` ; return; }"
                    "  const r=await fetch(`/api/modbus/raw/result?id=${encodeURIComponent(id)}`);"
                    "  const j=await r.json();"
                    "  qs('out').textContent = JSON.stringify(j,null,2);"
                    "  if(j.completed) return;"
                    "  setTimeout(()=>pollResult(id,n+1), 250);"
                    "}"
                    "</script></body></html>");

                request->send(200, "text/html", html);
            });

        // Decoded value viewer page
        webServer->on("/view/modbus/decoded", HTTP_GET,
            [&server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                String html = F("<!DOCTYPE html><html><head>"
                    "<title>Modbus Decoded Register Viewer</title>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<style>"
                    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial;margin:10px;background:#1a1a2e;color:#eee}"
                    ".card{background:#16213e;border-radius:8px;padding:10px;margin:10px 0;box-shadow:0 2px 4px rgba(0,0,0,0.3);width:max-content;min-width:100%}"
                    "h1{color:#4CAF50}h2{color:#eee;margin:0 0 10px 0}h3{color:#ccc;margin:10px 0 5px 0}"
                    "a{color:#4CAF50}"
                    "label{display:inline-block;margin:6px 10px 6px 0;font-weight:600}"
                    "select,input{padding:8px;background:#0f3460;color:#eee;border:1px solid #2a2a4a;border-radius:4px;min-width:200px}"
                    "button{padding:8px 16px;background:#4CAF50;color:#1a1a2e;border:none;border-radius:4px;font-weight:600;cursor:pointer;margin-top:10px}"
                    "button:hover{background:#45a049}"
                    ".result-box{background:#0a0a1a;border:2px solid #4CAF50;border-radius:6px;padding:15px;margin-top:15px}"
                    ".value{font-size:32px;font-weight:bold;color:#4CAF50;margin:10px 0}"
                    ".meta{color:#888;font-size:12px;margin-top:5px}"
                    "small{color:#888}"
                    ".error{color:#F44336}"
                    ".warning{color:#FF9800}"
                    "</style></head><body>"
                    "<h1>Decoded Register Viewer</h1>"
                    "<p><a href='/view/modbus'>&larr; Back to dashboard</a></p>"
                    "<div class='card'>"
                    "<h2>Read Decoded Value</h2>"
                    "<p><small>Select device and register to view decoded value from cache</small></p>"
                    "<div>"
                    "<div><label>Device<br><select id='device' onchange='updateRegisters()' style='width:100%'>"
                    "<option value=''>-- Select Device --</option>"
                    "</select></label></div>"
                    "<div><label>Register<br><select id='register' style='width:100%'>"
                    "<option value=''>-- Select Register --</option>"
                    "</select></label></div>"
                    "<div><label>Unit Multiplier<br><select id='unitMult' style='width:100%'>"
                    "<option value='1' selected>1 (original unit)</option>"
                    "<option value='0.001'>0.001 (k-unit, e.g. kW, kWh)</option>"
                    "<option value='1000'>1000 (m-unit, e.g. mV, mA)</option>"
                    "<option value='0.000001'>0.000001 (M-unit, e.g. MW)</option>"
                    "</select></label></div>"
                    "<div style='margin-top:10px'>"
                    "<label><input type='checkbox' id='autoRefresh' checked onchange='toggleAutoRefresh()'> Auto-refresh (5s)</label>"
                    "</div>"
                    "<button onclick='readValue()'>Read Value</button>"
                    "</div>"
                    "<div id='result' style='display:none'></div>"
                    "<div style='margin-top:8px;font-size:12px;color:#666'>"
                    "Last update: <span id='lastUpdate'>-</span>"
                    "</div>"
                    "</div>"
                    "<script>"
                    "let devicesData = [];"
                    "let currentRegisters = [];"
                    "let autoRefreshTimer = null;"
                    "function qs(id){return document.getElementById(id);}"
                    "function formatAge(sec){"
                    "  if(sec < 60) return sec+'s';"
                    "  const m = Math.floor(sec/60);"
                    "  if(m < 60) return m+'m '+(sec%60)+'s';"
                    "  const h = Math.floor(m/60);"
                    "  if(h < 24) return h+'h '+(m%60)+'m';"
                    "  const d = Math.floor(h/24);"
                    "  return d+'d '+(h%24)+'h';"
                    "}"
                    "async function init(){"
                    "  try{"
                    "    const r = await fetch('/api/modbus/registers');"
                    "    devicesData = await r.json();"
                    "    const devSel = qs('device');"
                    "    devSel.innerHTML = '<option value=\"\">-- Select Device --</option>';"
                    "    devicesData.forEach(d=>{"
                    "      const opt = document.createElement('option');"
                    "      opt.value = d.unitId;"
                    "      opt.textContent = `Unit ${d.unitId} - ${d.deviceName || d.deviceType}`;"
                    "      devSel.appendChild(opt);"
                    "    });"
                    "  }catch(e){"
                    "    alert('Failed to load devices: '+e.message);"
                    "  }"
                    "}"
                    "function updateRegisters(){"
                    "  stopAutoRefresh();"
                    "  const unitId = parseInt(qs('device').value);"
                    "  const regSel = qs('register');"
                    "  regSel.innerHTML = '<option value=\"\">-- Select Register --</option>';"
                    "  qs('result').style.display = 'none';"
                    "  if(!unitId) return;"
                    "  const dev = devicesData.find(d=>d.unitId===unitId);"
                    "  if(!dev || !dev.registers) return;"
                    "  currentRegisters = dev.registers;"
                    "  dev.registers.forEach(r=>{"
                    "    const opt = document.createElement('option');"
                    "    opt.value = r.name;"
                    "    opt.textContent = `${r.name} (${r.unit || 'no unit'}) @ ${r.address}`;"
                    "    regSel.appendChild(opt);"
                    "  });"
                    "}"
                    "async function readValue(){"
                    "  const unitId = qs('device').value;"
                    "  const regName = qs('register').value;"
                    "  const multStr = qs('unitMult').value;"
                    "  if(!unitId || !regName){"
                    "    alert('Please select device and register');"
                    "    return;"
                    "  }"
                    "  const mult = parseFloat(multStr) || 1.0;"
                    "  try{"
                    "    const r = await fetch(`/api/modbus/read?unit=${encodeURIComponent(unitId)}&register=${encodeURIComponent(regName)}`);"
                    "    const j = await r.json();"
                    "    const reg = currentRegisters.find(x=>x.name===regName);"
                    "    let unitStr = reg ? reg.unit : '';"
                    "    if(mult !== 1.0){"
                    "      if(mult === 0.001) unitStr = 'k'+unitStr;"
                    "      else if(mult === 1000) unitStr = 'm'+unitStr;"
                    "      else if(mult === 0.000001) unitStr = 'M'+unitStr;"
                    "      else unitStr = unitStr + ' × ' + mult;"
                    "    }"
                    "    const displayVal = j.value * mult;"
                    "    let html = '<div class=\"result-box\">';"
                    "    html += '<h3>'+regName+'</h3>';"
                    "    html += '<div class=\"value\">'+displayVal.toFixed(4)+' '+unitStr+'</div>';"
                    "    html += '<div class=\"meta\">Unit ID: '+j.unitId+'</div>';"
                    "    if(j.valid){"
                    "      html += '<div class=\"meta\">Valid: <span style=\"color:#4CAF50\">Yes</span>';"
                    "      if(j.ageSeconds !== undefined){"
                    "        html += ' (age: '+formatAge(j.ageSeconds)+')';"
                    "      }"
                    "      html += '</div>';"
                    "    }else{"
                    "      html += '<div class=\"meta\">Valid: <span class=\"error\">No</span>';"
                    "      if(j.ageSeconds !== undefined){"
                    "        html += ' <small>(last successful read: '+formatAge(j.ageSeconds)+' ago)</small>';"
                    "      }else{"
                    "        html += ' <small>(never read)</small>';"
                    "      }"
                    "      html += '</div>';"
                    "    }"
                    "    html += '<div class=\"meta\">Queued for update: '+(j.queued?'Yes':'No')+'</div>';"
                    "    html += '<div class=\"meta\">Original value: '+j.value.toFixed(4)+' '+(reg?reg.unit:'')+'</div>';"
                    "    if(reg){"
                    "      html += '<div class=\"meta\">Address: '+reg.address+', Length: '+reg.length+'</div>';"
                    "    }"
                    "    html += '</div>';"
                    "    qs('result').innerHTML = html;"
                    "    qs('result').style.display = 'block';"
                    "    qs('lastUpdate').textContent = new Date().toLocaleTimeString();"
                    "    startAutoRefresh();"
                    "  }catch(e){"
                    "    qs('result').innerHTML = '<div class=\"result-box\"><div class=\"error\">Error: '+e.message+'</div></div>';"
                    "    qs('result').style.display = 'block';"
                    "    qs('lastUpdate').textContent = new Date().toLocaleTimeString();"
                    "  }"
                    "}"
                    "function startAutoRefresh(){"
                    "  stopAutoRefresh();"
                    "  if(!qs('autoRefresh').checked) return;"
                    "  const unitId = qs('device').value;"
                    "  const regName = qs('register').value;"
                    "  if(!unitId || !regName) return;"
                    "  autoRefreshTimer = setInterval(()=>{"
                    "    if(qs('autoRefresh').checked && qs('device').value && qs('register').value){"
                    "      readValue();"
                    "    }else{"
                    "      stopAutoRefresh();"
                    "    }"
                    "  }, 5000);"
                    "}"
                    "function stopAutoRefresh(){"
                    "  if(autoRefreshTimer){"
                    "    clearInterval(autoRefreshTimer);"
                    "    autoRefreshTimer = null;"
                    "  }"
                    "}"
                    "function toggleAutoRefresh(){"
                    "  if(qs('autoRefresh').checked){"
                    "    startAutoRefresh();"
                    "  }else{"
                    "    stopAutoRefresh();"
                    "  }"
                    "}"
                    "init();"
                    "</script></body></html>");

                request->send(200, "text/html", html);
            });

        webServer->on("/view/modbus", HTTP_GET,
            [&devices, &modbus, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                // Static HTML shell - data loaded via AJAX
                String html = F("<!DOCTYPE html><html><head>"
                    "<title>Modbus Dashboard</title>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<style>"
                    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial;margin:10px;background:#1a1a2e;color:#eee}"
                    ".card{background:#16213e;border-radius:8px;padding:10px;margin:10px 0;box-shadow:0 2px 4px rgba(0,0,0,0.3);width:max-content;min-width:100%}"
                    ".device{border-left:4px solid #2196F3}"
                    ".status{border-left:4px solid #4CAF50}"
                    "table{width:100%;border-collapse:collapse;background:#16213e}"
                    "th,td{padding:6px 8px;text-align:left;border-bottom:1px solid #2a2a4a}"
                    "th{background:#0f3460;color:#2196F3}"
                    ".ok{color:#4CAF50}.err{color:#F44336}"
                    ".outdated{opacity:0.4;color:#666}"
                    "h1{color:#2196F3}h2{color:#eee;margin:0 0 10px 0}"
                    ".update-info{font-size:12px;color:#666;margin-top:10px}"
                    ".flash{animation:flash 0.3s}"
                    "@keyframes flash{0%{background:#2a4a6a}100%{background:transparent}}"
                    "</style></head><body>"
                    "<h1>Modbus Dashboard</h1>"
                    "<div id='content'>Loading...</div>"
                    "<div class='update-info'>Auto-refresh every 5s | Last update: <span id='lastUpdate'>-</span></div>"
                    "<script>"
                    "let prev={};"
                    "function render(d){"
                    "let h='<div class=\"card status\"><h2>Bus Status</h2><p>';"
                    "h+='Silent: <span class=\"'+(d.status.silent?'ok':'err')+'\">'+(d.status.silent?'Yes':'No')+'</span>';"
                    "h+=' | Queue: '+d.status.queue+' | RX: '+d.status.rx+' | TX: '+d.status.tx+' | CRC Errors: '+d.status.crcErrors+'</p></div>';"
                    "d.devices.forEach(dev=>{"
                    "h+='<div class=\"card device\"><h2>Unit '+dev.unitId+' - '+dev.type+'</h2>';"
                    "h+='<p>Success: '+dev.success+' | Errors: '+dev.errors+'</p>';"
                    "h+='<table><tr><th>Register</th><th>Value</th><th>Unit</th><th>Valid</th></tr>';"
                    "dev.values.forEach(v=>{"
                    "let key=dev.unitId+'_'+v.n;"
                    "let changed=prev[key]!==undefined&&prev[key]!==v.v;"
                    "prev[key]=v.v;"
                    "h+='<tr'+(v.old?' class=\"outdated\" title=\"Outdated\"':'')+'>';"
                    "h+='<td>'+v.n+'</td><td'+(changed?' class=\"flash\"':'')+'>'+v.v.toFixed(2)+'</td>';"
                    "h+='<td>'+v.u+'</td><td class=\"'+(v.ok?'ok':'err')+'\">'+(v.ok?'\\u2713':'\\u2717')+'</td></tr>';"
                    "});"
                    "h+='</table></div>';"
                    "});"
                    "document.getElementById('content').innerHTML=h;"
                    "document.getElementById('lastUpdate').textContent=new Date().toLocaleTimeString();"
                    "}"
                    "function fetchData(){fetch('/api/modbus/dashboard').then(r=>r.json()).then(render).catch(e=>console.error(e));}"
                    "fetchData();setInterval(fetchData,5000);"
                    "</script>"
                    "<div style='margin-top:12px;font-size:13px'>"
                    "<a href='/view/modbus/patterns' style='color:#4fc3f7'>Pattern Analysis</a> | "
                    "<a href='/view/modbus/scheduler' style='color:#4fc3f7'>Gap Scheduler</a> | "
                    "<a href='/view/modbus/raw' style='color:#4fc3f7'>Raw Tools</a> | "
                    "<a href='/view/modbus/decoded' style='color:#4fc3f7'>Decoded Viewer</a>"
                    "</div>"
                    "</body></html>");

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
                modbus.forEachRecentFrame([&](const ModbusFrame& frame) {
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
                    f["dataHex"] = modbus.formatHex(frame.data.data(), frame.dataLen);

                    // Split out common Modbus RTU FC3/FC4 fields.
                    if (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS) {
                        if (frame.isRequest && frame.dataLen == 4) {
                            f["startRegister"] = frame.getStartRegister();
                            f["quantity"] = frame.getQuantity();
                        } else if (!frame.isRequest && !frame.isException && frame.dataLen >= 1) {
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
                });

                String output;
                serializeJson(doc, output);
                request->send(200, "application/json", output);
            });

        // Get register definitions for all devices
        webServer->on("/api/modbus/registers", HTTP_GET,
            [&devices, &server](AsyncWebServerRequest* request) {
                if (!server.authenticate(request)) return request->requestAuthentication();

                auto _guard = devices.scopedLock();

                JsonDocument doc;
                JsonArray devicesArr = doc.to<JsonArray>();
                
                for (const auto& kv : devices.getDevices()) {
                    JsonObject devObj = devicesArr.add<JsonObject>();
                    devObj["unitId"] = kv.first;
                    devObj["deviceName"] = kv.second.deviceName;
                    devObj["deviceType"] = kv.second.deviceTypeName;
                    
                    JsonArray regsArr = devObj["registers"].to<JsonArray>();
                    if (kv.second.deviceType) {
                        for (const auto& reg : kv.second.deviceType->registers) {
                            JsonObject regObj = regsArr.add<JsonObject>();
                            regObj["name"] = reg.name;
                            regObj["address"] = reg.address;
                            regObj["length"] = reg.length;
                            regObj["unit"] = reg.unit;
                            regObj["dataType"] = (uint8_t)reg.dataType;
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
