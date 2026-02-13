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
 * @param data Raw frame data (unit id, function code, payload..., crc)
 * @param length Length of the `data` buffer
 * @param frame Output frame structure to populate
 * @param timestampMs Monotonic timestamp in milliseconds for the frame
 * @param unixTimestamp Epoch seconds when available (0 if unknown)
 * @return true if parsing produced a `ModbusFrame` instance (may be invalid CRC)
 */
bool parseModbusFrame(const uint8_t* data, size_t length, ModbusFrame& frame, uint32_t timestampMs, uint32_t unixTimestamp);

/**
 * @brief Update a `ModbusRegisterMap` using a matched request/response pair
 * @param regMap Register map to update (address -> value)
 * @param request Request frame that initiated the read
 * @param response Response frame carrying register data
 * @param currentTimeMs Current time in milliseconds (used for lastUpdate)
 */
void updateModbusRegisterMap(ModbusRegisterMap& regMap, const ModbusFrame& request, const ModbusFrame& response, uint32_t currentTimeMs);

/**
 * @brief Try to parse a frame at a specific length from a buffer pointer
 * @param p Pointer to the buffer start
 * @param remaining Remaining bytes available at `p`
 * @param len Expected frame length to attempt
 * @param out Output frame when parsing succeeds
 * @param timestampMs Timestamp to assign to parsed frame
 * @param unixTimestamp Unix timestamp to assign
 * @return true if parsing succeeded and `out` was populated
 */
bool tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out, uint32_t timestampMs, uint32_t unixTimestamp);

/**
 * @brief Determine the length of a (potential) Modbus frame within a buffer
 * @param p Pointer to the buffer start
 * @param remaining Bytes remaining in the buffer
 * @param isRequest Output flag set to true when a request frame was detected
 * @param frameLen Output: determined frame length in bytes (including CRC)
 * @return true when frame length could be determined, false otherwise
 */
bool determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen);

} // namespace ModbusRTUHelper

#endif // MODBUS_RTU_FEATURE_HELPER_H