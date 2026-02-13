#include "main_helper.h"
#include <ArduinoJson.h>
#include "ModbusDevice.h"
#include "MQTTFeature.h"
#include "ModbusRTUFeature.h"
#include "LoggingFeature.h"
#include "ResetManager.h"

namespace MainHelper {

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
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/raw/read", out.c_str(), false);
        return;
    }

    // ACK immediately so callers know we accepted the command.
    ack["queued"] = true;
    ack["unitId"] = unitId;
    ack["address"] = address;
    ack["count"] = count;
    ack["functionCode"] = fc;
    String outAck;
    serializeJson(ack, outAck);
    mqtt.publishToBase("modbus/ack/raw/read", outAck.c_str(), false);

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
            String out;
            serializeJson(resp, out);
            mqtt.publishToBase("modbus/resp/raw/read", out.c_str(), false);
        });

    if (!queued) {
        JsonDocument nack;
        nack["id"] = idBuf;
        nack["queued"] = false;
        nack["error"] = "queue_failed";
        String out;
        serializeJson(nack, out);
        mqtt.publishToBase("modbus/ack/raw/read", out.c_str(), false);
    }
#endif
}

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
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/raw/write", out.c_str(), false);
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
                String out;
                serializeJson(resp, out);
                mqtt.publishToBase("modbus/resp/raw/write", out.c_str(), false);
            });
    } else if (doc["value"].is<uint32_t>() || doc["value"].is<int>()) {
        uint16_t value = (uint16_t)(doc["value"].as<uint32_t>() & 0xFFFF);
        queued = modbus.queueWriteSingleRegister(unitId, address, value,
            [&mqtt, idBuf](bool success, const ModbusFrame&) {
                JsonDocument resp;
                resp["id"] = idBuf;
                resp["success"] = success;
                String out;
                serializeJson(resp, out);
                mqtt.publishToBase("modbus/resp/raw/write", out.c_str(), false);
            });
    } else {
        ack["queued"] = false;
        ack["error"] = "missing_value";
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/raw/write", out.c_str(), false);
        return;
    }

    ack["queued"] = queued;
    ack["unitId"] = unitId;
    ack["address"] = address;
    ack["functionCode"] = fc;
    String outAck;
    serializeJson(ack, outAck);
    mqtt.publishToBase("modbus/ack/raw/write", outAck.c_str(), false);
#endif
}

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
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/write", out.c_str(), false);
        return;
    }

    if (!modbusDevices) {
        ack["queued"] = false;
        ack["error"] = "devices_unavailable";
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/write", out.c_str(), false);
        return;
    }

    bool queued = modbusDevices->writeRegister(unitId, regName, value,
        [&mqtt, idBuf](bool success) {
            JsonDocument resp;
            resp["id"] = idBuf;
            resp["success"] = success;
            String out;
            serializeJson(resp, out);
            mqtt.publishToBase("modbus/resp/write", out.c_str(), false);
        });

    ack["queued"] = queued;
    ack["unitId"] = unitId;
    ack["register"] = regName;
    ack["value"] = value;
    String outAck;
    serializeJson(ack, outAck);
    mqtt.publishToBase("modbus/ack/write", outAck.c_str(), false);
#endif
}

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
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/read", out.c_str(), false);
        return;
    }

    if (!modbusDevices) {
        ack["queued"] = false;
        ack["error"] = "devices_unavailable";
        String out;
        serializeJson(ack, out);
        mqtt.publishToBase("modbus/ack/read", out.c_str(), false);
        return;
    }

    // ACK (queued true if we accepted the request)
    ack["queued"] = true;
    ack["unitId"] = unitId;
    ack["register"] = regName;
    String outAck;
    serializeJson(ack, outAck);
    mqtt.publishToBase("modbus/ack/read", outAck.c_str(), false);

    bool queued = modbusDevices->readRegister(unitId, regName,
        [&mqtt, idBuf, regName](bool success, float value) {
            JsonDocument resp;
            resp["id"] = idBuf;
            resp["unitId"] = (uint32_t)0 + 0; // placeholder, unit echoed below if needed
            resp["register"] = regName;
            resp["success"] = success;
            if (success) resp["value"] = value;
            String out;
            serializeJson(resp, out);
            mqtt.publishToBase("modbus/resp/read", out.c_str(), false);
        });

    // Note: modbusDevices->readRegister returns immediately whether queued; ack already sent
    (void)queued;
#endif
}

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
    String out;
    serializeJson(resp, out);
    mqtt.publishToBase("modbus/resp/list_devices", out.c_str(), false);
}

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
        String out;
        serializeJson(nack, out);
        mqtt.publishToBase("modbus/resp/list_registers", out.c_str(), false);
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
        String out;
        serializeJson(nack, out);
        mqtt.publishToBase("modbus/resp/list_registers", out.c_str(), false);
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
        String outNack;
        serializeJson(nack, outNack);
        mqtt.publishToBase("modbus/resp/list_registers", outNack.c_str(), false);
    }
}

} // namespace MainHelper