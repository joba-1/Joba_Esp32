#ifndef MODBUS_DEVICE_HELPER_H
#define MODBUS_DEVICE_HELPER_H

#include "ModbusDevice.h"
#include <cstdint>
#include <vector>
#include <cstring>

namespace ModbusDeviceHelper {

/**
 * @brief Parse a data type string into ModbusDataType enum
 */
ModbusDataType parseModbusDataType(const char* str);

/**
 * @brief Convert raw Modbus register data to a float value
 * @param def Register definition
 * @param rawData Pointer to raw register data
 * @return Converted float value
 */
float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData);

/**
 * @brief Convert a float value to raw Modbus register data
 * @param def Register definition
 * @param value Float value to convert
 * @return Vector of raw register values
 */
std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value);

/**
 * @brief Apply a read response to update device register values
 * @param device Device instance to update
 * @param functionCode Function code of the response
 * @param pollIntervalMs Poll interval for this batch
 * @param startAddress Starting register address
 * @param response Modbus response frame
 * @param nowMs Current time in milliseconds
 * @param nowUnix Current Unix timestamp
 */
void applyModbusReadResponseToDevice(ModbusDeviceInstance& device,
                                     uint8_t functionCode,
                                     uint32_t pollIntervalMs,
                                     uint16_t startAddress,
                                     const ModbusFrame& response,
                                     uint32_t nowMs,
                                     uint32_t nowUnix);

} // namespace ModbusDeviceHelper

#endif // MODBUS_DEVICE_HELPER_H