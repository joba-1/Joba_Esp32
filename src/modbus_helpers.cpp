#include "modbus_helpers.h"
#include <sstream>
#include <iomanip>

uint16_t modbus_crc16(const uint8_t* data, size_t len) {
    static uint16_t table[256];
    static bool table_init = false;
    if (!table_init) {
        for (int i = 0; i < 256; ++i) {
            uint16_t crc = (uint16_t)i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
                else crc = (uint16_t)(crc >> 1);
            }
            table[i] = crc;
        }
        table_init = true;
    }

    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < len; pos++) {
        uint8_t idx = (uint8_t)(crc ^ data[pos]);
        crc = (uint16_t)((crc >> 8) ^ table[idx]);
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
