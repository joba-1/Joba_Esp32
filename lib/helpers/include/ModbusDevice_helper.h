#ifndef MODBUS_DEVICE_HELPER_H
#define MODBUS_DEVICE_HELPER_H

#include "ModbusDevice.h"
#include <cstdint>
#include <vector>
#include <cstring>

namespace ModbusDeviceHelper {

ModbusDataType parseModbusDataType(const char* str);

float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData);

std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value);

void applyModbusReadResponseToDevice(ModbusDeviceInstance& device,
                                     uint8_t functionCode,
                                     uint32_t pollIntervalMs,
                                     uint16_t startAddress,
                                     const ModbusFrame& response,
                                     uint32_t nowMs,
                                     uint32_t nowUnix);

} // namespace ModbusDeviceHelper

#endif // MODBUS_DEVICE_HELPER_H
