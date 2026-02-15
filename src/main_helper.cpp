#include "main_helper.h"
#include <ArduinoJson.h>
#include "ModbusDevice.h"
#include "MQTTFeature.h"
#include "ModbusRTUFeature.h"
#include "LoggingFeature.h"
#include "ResetManager.h"

namespace MainHelper {
/**
 * @brief Handle an MQTT-triggered reset command.
 * @param payload MQTT payload string (accepted: "1", "true", "reset", ...).
 * @param resetTopic Topic the command arrived on.
 * @param mqtt MQTTFeature used to publish acknowledgment/result.
 */
void handleResetCommand(const String& payload, const String& resetTopic, MQTTFeature& mqtt) {
    String p(payload);
    p.trim();
    p.toLowerCase();
    if (p != "1" && p != "true" && p != "reset" && p != "restart" && p != "reboot") {
        LOG_W("MQTT reset ignored (payload='%s')", payload.c_str());
        return;
    }

    const bool scheduled = ResetManager::scheduleRestart(250, "mqtt");
    mqtt.publishToBase("status/reset", scheduled ? "scheduled" : "already_scheduled", false);
}

/**
 * @brief Parse and execute a raw Modbus read command received via MQTT.
 * @param payload JSON payload with fields like `unit`, `address`, `count`, `fc`.
 * @param modbusRawReadTopic Topic for raw read commands.
 * @param mqtt MQTTFeature used to send ACK/response messages.
 * @param modbus ModbusRTUFeature used to queue the actual Modbus read.
 */
void handleModbusRawReadCommand(const String& payload, const String& modbusRawReadTopic,
                               MQTTFeature& mqtt, ModbusRTUFeature& modbus) {
#if MODBUS_LISTEN_ONLY
    mqtt.publishToBase("modbus/ack/raw/read", "{\"queued\":false,\"error\":\"listen_only\"}", false);
    return;
#else
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    // Use fixed buffer for id to avoid heap-allocating a String in the lambda capture
    char idBuf[32];
    if (doc["id"].is<const char*>()) {
        strncpy(idBuf, doc["id"].as<const char*>(), sizeof(idBuf) - 1);
        idBuf[sizeof(idBuf) - 1] = '\0';
    } else {
        snprintf(idBuf, sizeof(idBuf), "%u", (uint32_t)millis());
    }
    uint8_t unitId = doc["unit"] | 0;
    uint16_t address = doc["address"] | 0;
    uint16_t count = doc["count"] | 0;
    uint8_t fc = doc["fc"] | 3;

    JsonDocument ack;
    ack["id"] = idBuf;
    ack["topic"] = (const char*)modbusRawReadTopic.c_str();

    if (err || unitId == 0 || count == 0) {
        ack["queued"] = false;
        ack["error"] = err ? "invalid_json" : "invalid_params";
        char outBuf[256];
        size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/ack/raw/read", outBuf, false);
        return;
    }

    // ACK immediately so callers know we accepted the command.
    ack["queued"] = true;
    ack["unitId"] = unitId;
    ack["address"] = address;
    ack["count"] = count;
    ack["functionCode"] = fc;
    {
        char outBuf[256];
        size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/ack/raw/read", outBuf, false);
    }

    bool queued = modbus.queueReadRegisters(
        unitId, fc, address, count,
        [&mqtt, idBuf](bool success, const ModbusFrame& response) {
            JsonDocument resp;
            resp["id"] = idBuf;
            resp["unitId"] = response.unitId;
            resp["functionCode"] = response.functionCode;
            resp["success"] = success;
            resp["isException"] = response.isException;
            if (response.isException) resp["exceptionCode"] = response.exceptionCode;
            {
                char crcHex[7];
                snprintf(crcHex, sizeof(crcHex), "0x%04X", (unsigned)response.crc);
                resp["crcHex"] = crcHex;
            }

            // Raw payload hex (no unit/fc/crc)
            // Note: We can't call modbus.formatHex here since modbus is not in lambda scope
            // This is a limitation of moving this to a helper function
            // For now, we'll skip this part or need to pass modbus reference to lambda
            // resp["dataHex"] = modbus.formatHex(response.data.data(), response.dataLen);

            uint8_t fcBase = response.functionCode & 0x7F;
            if (!response.isException && (fcBase == 3 || fcBase == 4)) { // READ_HOLDING_REGISTERS || READ_INPUT_REGISTERS
                const size_t byteCount = response.getByteCount();
                const uint8_t* regData = response.getRegisterData();
                resp["byteCount"] = (uint32_t)byteCount;
                if (regData && byteCount >= 2) {
                    // resp["registerDataHex"] = modbus.formatHex(regData, byteCount);

                    JsonArray words = resp["registerWords"].to<JsonArray>();
                    size_t wordCount = byteCount / 2;
                    static constexpr size_t MAX_WORDS = 32;
                    size_t emitCount = wordCount > MAX_WORDS ? MAX_WORDS : wordCount;
                    for (size_t i = 0; i < emitCount; i++) {
                        size_t idx = i * 2;
                        uint16_t w = ((uint16_t)regData[idx] << 8) | (uint16_t)regData[idx + 1];
                        words.add(w);
                    }
                    if (wordCount > MAX_WORDS) {
                        resp["registerWordsTruncated"] = true;
                        resp["registerWordCount"] = (uint32_t)wordCount;
                    }
                }
            }

            resp["uptimeMs"] = (uint32_t)millis();
            {
                char outBuf[256];
                size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
                outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
                mqtt.publishToBase("modbus/resp/raw/read", outBuf, false);
            }
        });

    if (!queued) {
        JsonDocument nack;
        nack["id"] = idBuf;
        nack["queued"] = false;
        nack["error"] = "queue_failed";
        {
            char outBuf[256];
            size_t outLen = serializeJson(nack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/raw/read", outBuf, false);
        }
    }
#endif
}

/**
 * @brief Parse and execute a raw Modbus write command received via MQTT.
 * @param payload JSON payload describing `unit`, `address` and `value(s)`.
 * @param modbusRawWriteTopic Topic for raw write commands.
 * @param mqtt MQTTFeature used to send ACK/response messages.
 * @param modbus ModbusRTUFeature used to queue the actual Modbus write.
 */
void handleModbusRawWriteCommand(const String& payload, const String& modbusRawWriteTopic,
                                MQTTFeature& mqtt, ModbusRTUFeature& modbus) {
#if MODBUS_LISTEN_ONLY
    mqtt.publishToBase("modbus/ack/raw/write", "{\"queued\":false,\"error\":\"listen_only\"}", false);
    return;
#else
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    char idBuf[32];
    if (doc["id"].is<const char*>()) {
        strncpy(idBuf, doc["id"].as<const char*>(), sizeof(idBuf) - 1);
        idBuf[sizeof(idBuf) - 1] = '\0';
    } else {
        snprintf(idBuf, sizeof(idBuf), "%u", (uint32_t)millis());
    }

    uint8_t unitId = doc["unit"] | 0;
    uint16_t address = doc["address"] | 0;
    uint8_t fc = doc["fc"] | 6; // default to single-write

    JsonDocument ack;
    ack["id"] = idBuf;
    ack["topic"] = (const char*)modbusRawWriteTopic.c_str();

    if (err || unitId == 0) {
        ack["queued"] = false;
        ack["error"] = err ? "invalid_json" : "invalid_params";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/raw/write", outBuf, false);
        }
        return;
    }

    // Determine single vs multiple
    bool queued = false;
    if (doc["values"].is<JsonArray>()) {
        JsonArray arr = doc["values"].as<JsonArray>();
        std::vector<uint16_t> values;
        for (uint32_t i = 0; i < arr.size(); ++i) {
            values.push_back((uint16_t)(arr[i].as<uint32_t>() & 0xFFFF));
        }
        queued = modbus.queueWriteMultipleRegisters(unitId, address, values,
            [&mqtt, idBuf](bool success, const ModbusFrame&) {
                JsonDocument resp;
                resp["id"] = idBuf;
                resp["success"] = success;
                        {
                            char outBuf[256];
                            size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
                            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
                            mqtt.publishToBase("modbus/resp/raw/write", outBuf, false);
                        }
            });
    } else if (doc["value"].is<uint32_t>() || doc["value"].is<int>()) {
        uint16_t value = (uint16_t)(doc["value"].as<uint32_t>() & 0xFFFF);
        queued = modbus.queueWriteSingleRegister(unitId, address, value,
            [&mqtt, idBuf](bool success, const ModbusFrame&) {
                JsonDocument resp;
                resp["id"] = idBuf;
                resp["success"] = success;
                {
                    char outBuf[256];
                    size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
                    outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
                    mqtt.publishToBase("modbus/resp/raw/write", outBuf, false);
                }
            });
    } else {
        ack["queued"] = false;
        ack["error"] = "missing_value";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/raw/write", outBuf, false);
        }
        return;
    }

    ack["queued"] = queued;
    ack["unitId"] = unitId;
    ack["address"] = address;
    ack["functionCode"] = fc;
    {
        char outBuf[256];
        size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/ack/raw/write", outBuf, false);
    }
#endif
}

/**
 * @brief Handle a high-level Modbus write command that targets named registers.
 * @param payload JSON payload containing `unit`, `register` and `value`.
 * @param modbusWriteTopic Topic where write command was received.
 * @param mqtt MQTTFeature used to send ACK/response messages.
 * @param modbusDevices ModbusDeviceManager for resolving device/register by name.
 */
void handleModbusWriteCommand(const String& payload, const String& modbusWriteTopic,
                             MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices) {
#if MODBUS_LISTEN_ONLY
    mqtt.publishToBase("modbus/ack/write", "{\"queued\":false,\"error\":\"listen_only\"}", false);
    return;
#else
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    char idBuf[32];
    if (doc["id"].is<const char*>()) {
        strncpy(idBuf, doc["id"].as<const char*>(), sizeof(idBuf) - 1);
        idBuf[sizeof(idBuf) - 1] = '\0';
    } else {
        snprintf(idBuf, sizeof(idBuf), "%u", (uint32_t)millis());
    }

    uint8_t unitId = doc["unit"] | 0;
    const char* regName = doc["register"].is<const char*>() ? doc["register"].as<const char*>() : nullptr;
    float value = doc["value"] | 0.0f;

    JsonDocument ack;
    ack["id"] = idBuf;
    ack["topic"] = (const char*)modbusWriteTopic.c_str();

    if (err || unitId == 0 || regName == nullptr) {
        ack["queued"] = false;
        ack["error"] = err ? "invalid_json" : "invalid_params";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/write", outBuf, false);
        }
        return;
    }

    if (!modbusDevices) {
        ack["queued"] = false;
        ack["error"] = "devices_unavailable";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/write", outBuf, false);
        }
        return;
    }

    bool queued = modbusDevices->writeRegister(unitId, regName, value,
        [&mqtt, idBuf](bool success) {
            JsonDocument resp;
            resp["id"] = idBuf;
            resp["success"] = success;
            char outBuf[256];
            size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/resp/write", outBuf, false);
        });

    ack["queued"] = queued;
    ack["unitId"] = unitId;
    ack["register"] = regName;
    ack["value"] = value;
    {
        char outBuf[256];
        size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/ack/write", outBuf, false);
    }
#endif
}

/**
 * @brief Handle a high-level Modbus read command that targets named registers.
 * @param payload JSON payload containing `unit` and `register` to read.
 * @param modbusReadTopic Topic where read command was received.
 * @param mqtt MQTTFeature used to send ACK/response messages.
 * @param modbusDevices ModbusDeviceManager for resolving device/register by name.
 */
void handleModbusReadCommand(const String& payload, const String& modbusReadTopic,
                            MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices) {
#if MODBUS_LISTEN_ONLY
    mqtt.publishToBase("modbus/ack/read", "{\"queued\":false,\"error\":\"listen_only\"}", false);
    return;
#else
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    char idBuf[32];
    if (doc["id"].is<const char*>()) {
        strncpy(idBuf, doc["id"].as<const char*>(), sizeof(idBuf) - 1);
        idBuf[sizeof(idBuf) - 1] = '\0';
    } else {
        snprintf(idBuf, sizeof(idBuf), "%u", (uint32_t)millis());
    }

    uint8_t unitId = doc["unit"] | 0;
    const char* regName = doc["register"].is<const char*>() ? doc["register"].as<const char*>() : nullptr;

    JsonDocument ack;
    ack["id"] = idBuf;
    ack["topic"] = (const char*)modbusReadTopic.c_str();

    if (err || unitId == 0 || regName == nullptr) {
        ack["queued"] = false;
        ack["error"] = err ? "invalid_json" : "invalid_params";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/read", outBuf, false);
        }
        return;
    }

    if (!modbusDevices) {
        ack["queued"] = false;
        ack["error"] = "devices_unavailable";
        {
            char outBuf[256];
            size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/ack/read", outBuf, false);
        }
        return;
    }

    // ACK (queued true if we accepted the request)
    ack["queued"] = true;
    ack["unitId"] = unitId;
    ack["register"] = regName;
    {
        char outBuf[256];
        size_t outLen = serializeJson(ack, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/ack/read", outBuf, false);
    }

    bool queued = modbusDevices->readRegister(unitId, regName,
        [&mqtt, idBuf, regName](bool success, float value) {
            JsonDocument resp;
            resp["id"] = idBuf;
            resp["unitId"] = (uint32_t)0 + 0; // placeholder, unit echoed below if needed
            resp["register"] = regName;
            resp["success"] = success;
            if (success) resp["value"] = value;
            {
                char outBuf[256];
                size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
                outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
                mqtt.publishToBase("modbus/resp/read", outBuf, false);
            }
        });

    // Note: modbusDevices->readRegister returns immediately whether queued; ack already sent
    (void)queued;
#endif
}

/**
 * @brief Publish a list of known Modbus devices over MQTT.
 * @param mqtt MQTTFeature used for publishing.
 * @param modbusDevices ModbusDeviceManager providing device information.
 */
void handleModbusListDevicesCommand(MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices) {
    if (!modbusDevices) {
        mqtt.publishToBase("modbus/resp/list_devices", "{\"error\":\"devices_unavailable\"}", false);
        return;
    }
    auto _guard = modbusDevices->scopedLock();
    JsonDocument resp;
    JsonArray arr = resp.to<JsonArray>();
    for (const auto& kv : modbusDevices->getDevices()) {
        const auto& d = kv.second;
        JsonObject o = arr.add<JsonObject>();
        o["unitId"] = d.unitId;
        o["deviceName"] = d.deviceName.c_str();
        o["deviceType"] = d.deviceTypeName.c_str();
        o["successCount"] = d.successCount;
        o["errorCount"] = d.errorCount;
    }
    // Estimate payload size and avoid heap allocation where possible.
    size_t needed = measureJson(resp);
    if (needed + 1 <= 256) {
        char outBuf[256];
        size_t outLen = serializeJson(resp, outBuf, sizeof(outBuf));
        outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
        mqtt.publishToBase("modbus/resp/list_devices", outBuf, false);
    } else {
        // For larger payloads, allocate exact-size buffer and publish via publishLarge
        char* bigBuf = new char[needed + 1];
        size_t outLen = serializeJson(resp, bigBuf, needed + 1);
        bigBuf[(outLen < needed + 1) ? outLen : needed] = '\0';
        mqtt.publishLarge("modbus/resp/list_devices", bigBuf, false);
        delete[] bigBuf;
    }
}

/**
 * @brief Publish the register list for a given device over MQTT.
 * @param payload Payload containing `id` and `unit` fields to identify request.
 * @param mqtt MQTTFeature used for publishing.
 * @param modbusDevices ModbusDeviceManager providing device/register metadata.
 */
void handleModbusListRegistersCommand(const String& payload, MQTTFeature& mqtt,
                                     ModbusDeviceManager* modbusDevices) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    char idBuf[32];
    if (doc["id"].is<const char*>()) {
        strncpy(idBuf, doc["id"].as<const char*>(), sizeof(idBuf) - 1);
        idBuf[sizeof(idBuf) - 1] = '\0';
    } else {
        snprintf(idBuf, sizeof(idBuf), "%u", (uint32_t)millis());
    }

    uint8_t unitId = doc["unit"] | 0;
    LOG_I("MQTT handler: list_registers id=%s unit=%u", idBuf, (unsigned)unitId);
    if (err || unitId == 0) {
        JsonDocument nack;
        nack["id"] = idBuf;
        nack["queued"] = false;
        nack["error"] = err ? "invalid_json" : "invalid_params";
        {
            char outBuf[256];
            size_t outLen = serializeJson(nack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/resp/list_registers", outBuf, false);
        }
        return;
    }

    if (!modbusDevices) {
        mqtt.publishToBase("modbus/resp/list_registers", "{\"error\":\"devices_unavailable\"}", false);
        return;
    }

    auto _guard = modbusDevices->scopedLock();
    ModbusDeviceInstance* dev = modbusDevices->getDevice(unitId);
    if (!dev || !dev->deviceType) {
        JsonDocument nack;
        nack["id"] = idBuf;
        nack["error"] = "device_not_found";
        {
            char outBuf[256];
            size_t outLen = serializeJson(nack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/resp/list_registers", outBuf, false);
        }
        return;
    }

    JsonDocument resp;
    resp["id"] = idBuf;
    resp["unitId"] = unitId;
    JsonArray regs = resp["registers"].to<JsonArray>();
    for (const auto& r : dev->deviceType->registers) {
        JsonObject ro = regs.add<JsonObject>();
        ro["name"] = r.name;
        ro["address"] = r.address;
        ro["length"] = r.length;
        ro["functionCode"] = r.functionCode;
        ro["unit"] = r.unit;
    }
    String out;
    serializeJson(resp, out);
    LOG_I("Publishing list_registers response (unit=%u) size=%u", (unsigned)unitId, (unsigned)out.length());
    bool pubOk = mqtt.publishLarge("modbus/resp/list_registers", out.c_str(), false);
    if (!pubOk) {
        LOG_W("Failed to publish full list_registers response (unit=%u) size=%u", (unsigned)unitId, (unsigned)out.length());
        // Fallback: publish a short error so callers know it failed
        JsonDocument nack;
        nack["id"] = idBuf;
        nack["error"] = "publish_failed";
        {
            char outBuf[256];
            size_t outLen = serializeJson(nack, outBuf, sizeof(outBuf));
            outBuf[(outLen < sizeof(outBuf)) ? outLen : (sizeof(outBuf) - 1)] = '\0';
            mqtt.publishToBase("modbus/resp/list_registers", outBuf, false);
        }
    }
}

} // namespace MainHelper