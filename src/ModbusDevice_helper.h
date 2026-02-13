#ifndef MODBUS_DEVICE_HELPER_H
#define MODBUS_DEVICE_HELPER_H

#include "ModbusDevice.h"
#include <cstdint>
#include <vector>
#include <cstring>

namespace ModbusDeviceHelper {

/**
 * @brief Parse a Modbus data type string into the corresponding enum.
 * @param str Null-terminated C-string describing the data type (e.g. "float32_be").
 * @return Corresponding `ModbusDataType` value; returns `ModbusDataType::UINT16` as a safe default.
 */
ModbusDataType parseModbusDataType(const char* str);

/**
 * @brief Convert raw Modbus register words to a floating-point value using the register definition.
 * @param def Register definition describing data type, length and conversion factor
 * @param rawData Pointer to the first 16-bit register word for this value (big-endian order implied by def)
 * @return Converted `float` value (after applying factor & offset)
 */
float convertModbusRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData);

/**
 * @brief Convert a floating-point value into raw Modbus register words according to `def`.
 * @param def Register definition to determine target layout (endianness/length)
 * @param value Float value to convert
 * @return Vector of 16-bit register words ready to be written via Modbus
 */
std::vector<uint16_t> convertModbusValueToRaw(const ModbusRegisterDef& def, float value);

/**
 * @brief Apply a Modbus read response to a `ModbusDeviceInstance` by decoding register values
 * and updating the device's cached `ModbusRegisterValue` entries.
 * @param device Device instance to update
 * @param functionCode Modbus function code (e.g. 3 or 4)
 * @param pollIntervalMs Poll interval associated with this response in milliseconds
 * @param startAddress Starting register address from the response
 * @param response The raw `ModbusFrame` response containing register bytes
 * @param nowMs Current monotonic time in milliseconds (for `updatedAtMs`)
 * @param nowUnix Current Unix epoch seconds (0 if unavailable)
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