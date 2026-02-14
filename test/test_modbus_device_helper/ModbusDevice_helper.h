#ifndef TEST_MODBUS_DEVICE_HELPER_H
#define TEST_MODBUS_DEVICE_HELPER_H

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

// Prototypes inside namespace to match implementation
namespace ModbusDeviceHelper {
    ModbusDataType parseModbusDataType(const char* str);
    float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData);
    std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value);
}

#endif // TEST_MODBUS_DEVICE_HELPER_H
