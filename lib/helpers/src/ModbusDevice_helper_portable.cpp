#include "ModbusDevice_helper_portable.h"
#include <vector>
#include <string.h>

namespace ModbusDeviceHelper {

ModbusDataType parseModbusDataType(const char* str) {
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

    while (result.size() < def.length) {
        result.push_back(0);
    }

    return result;
}

} // namespace ModbusDeviceHelper
