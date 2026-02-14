#ifndef MODBUS_DEVICE_HELPER_PORTABLE_H
#define MODBUS_DEVICE_HELPER_PORTABLE_H

#include <cstdint>
#include <cstddef>
#include <vector>

enum class ModbusDataType : uint8_t {
    UINT16 = 0,
    INT16,
    UINT32_BE,
    UINT32_LE,
    INT32_BE,
    INT32_LE,
    FLOAT32_BE,
    FLOAT32_LE,
    BOOL,
    STRING
};

struct ModbusRegisterDef {
    char name[32];
    uint16_t address;
    uint16_t length;
    uint8_t functionCode;
    ModbusDataType dataType;
    float conversionFactor;
    float offset;
    char unit[16];
    uint32_t pollIntervalMs;
};

namespace ModbusDeviceHelper {
    ModbusDataType parseModbusDataType(const char* str);
    float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData);
    std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value);
}

#endif // MODBUS_DEVICE_HELPER_PORTABLE_H
