#include "ModbusRTUFeature_helper.h"
#include "ModbusFrame.h"
#include "modbus_helpers.h"
#include <cstring>
#include <algorithm>

static constexpr uint8_t MAX_RTU_UNIT_ID = 247;
static constexpr size_t MIN_FRAME_SIZE = 4;

namespace ModbusRTUHelper {

static inline bool isReadFunction(uint8_t fc) {
    return (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS);
}

bool parseModbusFrame(const uint8_t* data, size_t length, ModbusFrame& frame, uint32_t timestampMs, uint32_t unixTimestamp) {
    if (length < 4) return false;

    frame.unitId = data[0];
    frame.functionCode = data[1];
    frame.timestamp = timestampMs;
    frame.unixTimestamp = unixTimestamp;
    frame.isRequest = false;

    frame.timestamp = timestampMs;
    frame.unixTimestamp = unixTimestamp;

    uint16_t receivedCrc = (uint16_t)data[length - 2] | ((uint16_t)data[length - 1] << 8);
    uint16_t calculatedCrc = modbus_crc16(data, length - 2);

    const size_t payloadLenRaw = length - 4;
    const size_t payloadLen = (payloadLenRaw <= ModbusFrame::MAX_DATA_LEN) ? payloadLenRaw : ModbusFrame::MAX_DATA_LEN;

    if (receivedCrc != calculatedCrc) {
        frame.isValid = false;
        frame.crc = receivedCrc;
        frame.dataLen = (uint16_t)payloadLen;
        if (payloadLen > 0) memcpy(frame.data.data(), data + 2, payloadLen);
        frame.isException = false;
        frame.exceptionCode = 0;
        return true;
    }

    frame.crc = receivedCrc;
    frame.isValid = true;

    if (frame.functionCode & 0x80) {
        frame.isException = true;
        frame.exceptionCode = (length > 2) ? data[2] : 0;
        frame.dataLen = 0;
    } else {
        frame.isException = false;
        frame.exceptionCode = 0;
        frame.dataLen = (uint16_t)payloadLen;
        if (payloadLen > 0) memcpy(frame.data.data(), data + 2, payloadLen);
    }

    return true;
}

void updateModbusRegisterMap(ModbusRegisterMap& regMap, const ModbusFrame& request, const ModbusFrame& response, uint32_t currentTimeMs) {
    if (!response.isValid || response.isException) return;

    uint8_t fc = response.functionCode;
    if (fc != ModbusFC::READ_HOLDING_REGISTERS &&
        fc != ModbusFC::READ_INPUT_REGISTERS &&
        fc != ModbusFC::READ_COILS &&
        fc != ModbusFC::READ_DISCRETE_INPUTS) {
        return;
    }

    regMap.responseCount++;
    regMap.lastUpdate = currentTimeMs;

    uint16_t startReg = request.getStartRegister();
    size_t byteCount = response.getByteCount();
    const uint8_t* regData = response.getRegisterData();

    if (!regData || byteCount == 0) return;

    if (isReadFunction(fc)) {
        size_t regCount = byteCount / 2;
        auto it = regMap.registers.lower_bound(startReg);
        uint16_t addr = startReg;
        for (size_t i = 0; i < regCount; i++, ++addr) {
            uint16_t value = (regData[i * 2] << 8) | regData[i * 2 + 1];
            if (it != regMap.registers.end() && it->first == addr) {
                it->second = value;
                ++it;
            } else {
                it = regMap.registers.emplace_hint(it, addr, value);
                ++it;
            }
        }
    } else {
        auto it = regMap.registers.lower_bound(startReg);
        uint16_t addr = startReg;
        size_t bits = byteCount * 8;
        for (size_t i = 0; i < bits; i++, ++addr) {
            uint16_t value = (regData[i / 8] >> (i % 8)) & 0x01;
            if (it != regMap.registers.end() && it->first == addr) {
                it->second = value;
                ++it;
            } else {
                it = regMap.registers.emplace_hint(it, addr, value);
                ++it;
            }
        }
    }
}

bool tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out, uint32_t timestampMs, uint32_t unixTimestamp) {
    if (remaining < len) return false;
    return parseModbusFrame(p, len, out, timestampMs, unixTimestamp);
}

bool determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen) {
    isRequest = false;
    frameLen = 0;
    if (remaining < 4) return false;

    uint8_t fc = p[1];
    static constexpr uint8_t FC3 = ModbusFC::READ_HOLDING_REGISTERS;
    static constexpr uint8_t FC4 = ModbusFC::READ_INPUT_REGISTERS;
    static constexpr uint8_t FC3_EX = (uint8_t)(FC3 | 0x80);
    static constexpr uint8_t FC4_EX = (uint8_t)(FC4 | 0x80);
    static constexpr uint16_t MAX_REGS_PER_READ = 125;
    static constexpr uint8_t MAX_BYTECOUNT = 250;

    ModbusFrame tmp;

    if ((fc == FC3_EX || fc == FC4_EX) && remaining >= 5) {
        if (tryParseAtLen(p, remaining, 5, tmp, 0, 0) && tmp.isException) {
            isRequest = false;
            frameLen = 5;
            return true;
        }
    }

    if (fc == FC3 || fc == FC4) {
        // Try RESPONSE format FIRST: responses have a byteCount field that tells us
        // the exact expected length. This prevents misinterpreting truncated responses
        // as requests when the final CRC byte hasn't arrived yet.
        uint8_t byteCount = (remaining >= 3) ? p[2] : 0;
        bool couldBeResponse = (byteCount >= 2 && (byteCount % 2) == 0 && byteCount <= MAX_BYTECOUNT);
        size_t respLen = couldBeResponse ? ((size_t)byteCount + 5) : 0;
        
        if (couldBeResponse && remaining >= respLen) {
            if (tryParseAtLen(p, remaining, respLen, tmp, 0, 0) && !tmp.isException) {
                // Response structure matches; CRC validity checked later
                isRequest = false;
                frameLen = respLen;
                return true;
            }
        }
        // Try REQUEST format: 8-byte fixed length
        // Require CRC validity ONLY if this could be a truncated response (byteCount
        // indicates we need more bytes). This prevents matching 8 bytes of a 9-byte
        // response as a request.
        if (remaining >= 8) {
            if (tryParseAtLen(p, remaining, 8, tmp, 0, 0) && !tmp.isException && tmp.dataLen == 4) {
                uint16_t qty = tmp.getQuantity();
                if (qty >= 1 && qty <= MAX_REGS_PER_READ) {
                    // If byteCount could indicate a longer response but we only have 8 bytes,
                    // require CRC validity to avoid false request matches
                    bool requireCrcValid = couldBeResponse && (remaining < respLen || remaining == 8);
                    if (!requireCrcValid || tmp.isValid) {
                        isRequest = true;
                        frameLen = 8;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace ModbusRTUHelper
