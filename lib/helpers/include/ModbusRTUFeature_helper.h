#ifndef MODBUS_RTU_FEATURE_HELPER_H
#define MODBUS_RTU_FEATURE_HELPER_H

#include "ModbusFrame.h"
#include "ModbusFC.h"
#include "ModbusRegisterMap.h"
#include <cstdint>
#include <cstddef>

namespace ModbusRTUHelper {

bool parseModbusFrame(const uint8_t* data, size_t length, ModbusFrame& frame, uint32_t timestampMs, uint32_t unixTimestamp);

void updateModbusRegisterMap(ModbusRegisterMap& regMap, const ModbusFrame& request, const ModbusFrame& response, uint32_t currentTimeMs);

bool tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out, uint32_t timestampMs, uint32_t unixTimestamp);

bool determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen);

} // namespace ModbusRTUHelper

#endif // MODBUS_RTU_FEATURE_HELPER_H
