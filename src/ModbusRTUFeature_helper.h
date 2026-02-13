#ifndef MODBUS_RTU_FEATURE_HELPER_H
#define MODBUS_RTU_FEATURE_HELPER_H

#include "ModbusFrame.h"
#include "ModbusFC.h"
#include "ModbusRegisterMap.h"
#include <cstdint>
#include <cstddef>

namespace ModbusRTUHelper {

/**
 * @brief Parse a Modbus RTU frame from raw bytes
 * @param data Raw frame data
 * @param length Length of data
 * @param frame Output frame structure
 * @param timestampMs Current timestamp in milliseconds
 * @param unixTimestamp Current Unix timestamp
 * @return true if parsing succeeded (even for invalid CRC)
 */
bool parseModbusFrame(const uint8_t* data, size_t length, ModbusFrame& frame, uint32_t timestampMs, uint32_t unixTimestamp);

/**
 * @brief Update a register map with data from a request/response pair
 * @param regMap Register map to update
 * @param request The request frame
 * @param response The response frame
 * @param currentTimeMs Current time in milliseconds
 */
void updateModbusRegisterMap(ModbusRegisterMap& regMap, const ModbusFrame& request, const ModbusFrame& response, uint32_t currentTimeMs);

/**
 * @brief Try to parse a frame at a specific length
 * @param p Pointer to frame data
 * @param remaining Remaining bytes available
 * @param len Expected frame length
 * @param out Output frame
 * @param timestampMs Current timestamp
 * @param unixTimestamp Unix timestamp
 * @return true if parsing succeeded
 */
bool tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out, uint32_t timestampMs, uint32_t unixTimestamp);

/**
 * @brief Determine the length of a Modbus frame and whether it's a request
 * @param p Pointer to frame data
 * @param remaining Remaining bytes available
 * @param isRequest Output: true if this is a request frame
 * @param frameLen Output: frame length in bytes
 * @return true if frame length could be determined
 */
bool determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen);

} // namespace ModbusRTUHelper

#endif // MODBUS_RTU_FEATURE_HELPER_H