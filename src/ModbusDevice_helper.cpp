#include "ModbusDevice_helper.h"
#include <vector>
#include <string.h>

namespace ModbusDeviceHelper {

ModbusDataType parseModbusDataType(const char* str) {
    /**
     * @brief Parse a case-insensitive type name into a ModbusDataType enum.
     * @param str Null-terminated C string containing the type name (e.g. "uint32_be").
     * @return Corresponding ModbusDataType, or ModbusDataType::UINT16 as default.
     */
    if (strcasecmp(str, "int16") == 0) return ModbusDataType::INT16;
    if (strcasecmp(str, "uint32_be") == 0) return ModbusDataType::UINT32_BE;
    if (strcasecmp(str, "uint32_le") == 0) return ModbusDataType::UINT32_LE;
    if (strcasecmp(str, "int32_be") == 0) return ModbusDataType::INT32_BE;
    if (strcasecmp(str, "int32_le") == 0) return ModbusDataType::INT32_LE;
    if (strcasecmp(str, "float32_be") == 0) return ModbusDataType::FLOAT32_BE;
    if (strcasecmp(str, "float32_le") == 0) return ModbusDataType::FLOAT32_LE;
    if (strcasecmp(str, "bool") == 0) return ModbusDataType::BOOL;
    if (strcasecmp(str, "string") == 0) return ModbusDataType::STRING;
    return ModbusDataType::UINT16;  // Default
}

float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData) {
    /**
     * @brief Convert raw Modbus register words into a floating-point value.
     * @param def Register definition describing dataType, length, conversionFactor and offset.
     * @param rawData Pointer to an array of 16-bit words (big-endian on wire) to read from.
     * @return Converted floating value after applying conversionFactor and offset.
     */
    float rawValue = 0;

    switch (def.dataType) {
        case ModbusDataType::UINT16:
            rawValue = rawData[0];
            break;

        case ModbusDataType::INT16:
            rawValue = (int16_t)rawData[0];
            break;

        case ModbusDataType::UINT32_BE:
            rawValue = ((uint32_t)rawData[0] << 16) | rawData[1];
            break;

        case ModbusDataType::UINT32_LE:
            rawValue = ((uint32_t)rawData[1] << 16) | rawData[0];
            break;

        case ModbusDataType::INT32_BE: {
            int32_t val = ((uint32_t)rawData[0] << 16) | rawData[1];
            rawValue = val;
            break;
        }

        case ModbusDataType::INT32_LE: {
            int32_t val = ((uint32_t)rawData[1] << 16) | rawData[0];
            rawValue = val;
            break;
        }

        case ModbusDataType::FLOAT32_BE: {
            uint32_t bits = ((uint32_t)rawData[0] << 16) | rawData[1];
            memcpy(&rawValue, &bits, sizeof(float));
            break;
        }

        case ModbusDataType::FLOAT32_LE: {
            uint32_t bits = ((uint32_t)rawData[1] << 16) | rawData[0];
            memcpy(&rawValue, &bits, sizeof(float));
            break;
        }

        case ModbusDataType::BOOL:
            rawValue = rawData[0] ? 1.0f : 0.0f;
            break;

        default:
            rawValue = rawData[0];
            break;
    }

    return (rawValue * def.conversionFactor) + def.offset;
}

std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value) {
    /**
     * @brief Convert a floating-point `value` to a vector of Modbus 16-bit words
     *        suitable for writing, according to the register definition.
     * @param def Register definition describing target representation and length.
     * @param value The floating value to convert into raw register words.
     * @return Vector of 16-bit words padded to `def.length`.
     */
    // Apply reverse conversion
    float rawValue = (value - def.offset) / def.conversionFactor;

    std::vector<uint16_t> result;
    result.reserve(def.length);

    switch (def.dataType) {
        case ModbusDataType::UINT16:
            result.push_back((uint16_t)rawValue);
            break;

        case ModbusDataType::INT16:
            result.push_back((uint16_t)(int16_t)rawValue);
            break;

        case ModbusDataType::UINT32_BE:
        case ModbusDataType::INT32_BE: {
            uint32_t val = (uint32_t)rawValue;
            result.push_back((uint16_t)(val >> 16));
            result.push_back((uint16_t)(val & 0xFFFF));
            break;
        }

        case ModbusDataType::UINT32_LE:
        case ModbusDataType::INT32_LE: {
            uint32_t val = (uint32_t)rawValue;
            result.push_back((uint16_t)(val & 0xFFFF));
            result.push_back((uint16_t)(val >> 16));
            break;
        }

        case ModbusDataType::FLOAT32_BE: {
            uint32_t bits;
            memcpy(&bits, &rawValue, sizeof(float));
            result.push_back((uint16_t)(bits >> 16));
            result.push_back((uint16_t)(bits & 0xFFFF));
            break;
        }

        case ModbusDataType::FLOAT32_LE: {
            uint32_t bits;
            memcpy(&bits, &rawValue, sizeof(float));
            result.push_back((uint16_t)(bits & 0xFFFF));
            result.push_back((uint16_t)(bits >> 16));
            break;
        }

        case ModbusDataType::BOOL:
            result.push_back(rawValue != 0.0f ? 1 : 0);
            break;

        default:
            result.push_back((uint16_t)rawValue);
            break;
    }

    // Pad to required length if needed
    while (result.size() < def.length) {
        result.push_back(0);
    }

    return result;
}

void applyModbusReadResponseToDevice(ModbusDeviceInstance& device,
                                     uint8_t functionCode,
                                     uint32_t pollIntervalMs,
                                     uint16_t startAddress,
                                     const ModbusFrame& response,
                                     uint32_t nowMs,
                                     uint32_t nowUnix) {
    /**
     * @brief Apply a Modbus read response frame to the device's cached values.
     *
     * This will convert the response bytes to 16-bit words and update any
     * register entries in `device.currentValues` that fall into the returned
     * read window and match the function code and poll interval.
     *
     * @param device Device instance to update.
     * @param functionCode Modbus function code for the read (e.g. 0x03).
     * @param pollIntervalMs Poll interval associated with this batch read.
     * @param startAddress Start register address of the returned window.
     * @param response The ModbusFrame containing response bytes and metadata.
     * @param nowMs Current time in milliseconds (millis()).
     * @param nowUnix Current Unix timestamp in seconds, or 0 if unavailable.
     */
    if (!device.deviceType) return;
    if (!response.isValid || response.isException) return;

    const uint8_t* data = response.getRegisterData();
    size_t byteCount = response.getByteCount();
    if (!data || byteCount < 2) return;

    const uint16_t wordCount = (uint16_t)(byteCount / 2);

    // Convert response bytes into 16-bit words (big-endian on the wire)
    std::vector<uint16_t> words;
    words.reserve(wordCount);
    for (uint16_t i = 0; i < wordCount; i++) {
        uint16_t w = ((uint16_t)data[i * 2] << 8) | (uint16_t)data[i * 2 + 1];
        words.push_back(w);
    }

    // Update every register definition that is covered by this read window.
    for (const auto& reg : device.deviceType->registers) {
        // Apply pollIntervalFactor to match the adjusted batch interval
        uint32_t adjustedRegInterval = (uint32_t)(reg.pollIntervalMs * device.pollIntervalFactor);
        if (adjustedRegInterval != pollIntervalMs) continue;
        if (reg.functionCode != functionCode) continue;
        if (reg.dataType == ModbusDataType::STRING) continue;

        if (reg.address < startAddress) continue;
        uint32_t offset = (uint32_t)(reg.address - startAddress);
        if (offset + reg.length > wordCount) continue;

        float value = convertModbusRawToValue(reg, &words[offset]);

        auto& cached = device.currentValues[reg.name];
        cached.updatedAtMs = nowMs;
        cached.unixTimestamp = nowUnix;
        cached.timestamp = (nowUnix != 0) ? nowUnix : (nowMs / 1000);
        cached.value = value;
        cached.valid = true;

        // Note: notification is handled by the caller
    }
}

} // namespace ModbusDeviceHelper