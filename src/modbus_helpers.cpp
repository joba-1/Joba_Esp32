#include "modbus_helpers.h"
#include <sstream>
#include <iomanip>

/**
 * @file modbus_helpers.cpp
 * @brief Small utility implementations for Modbus CRC and hex formatting.
 *
 * Contains a compact CRC-16 (Modbus) implementation and a human-friendly
 * hex formatter used by logging and web/UI helpers. These functions are
 * standalone and intentionally allocation-minimal for embedded use.
 */

uint16_t modbus_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

std::string format_hex(const uint8_t* data, size_t len) {
    if (len == 0) return std::string();
    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        if (i) ss << ' ';
        ss << std::setw(2) << (int)(data[i] & 0xFF);
    }
    return ss.str();
}
