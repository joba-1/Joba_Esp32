#pragma once
#include <stdint.h>
#include <string>

// Calculate Modbus RTU CRC-16 (polynomial 0xA001)
uint16_t modbus_crc16(const uint8_t* data, size_t len);

// Format a byte buffer as hex bytes separated by spaces, e.g. "01 02 AB"
std::string format_hex(const uint8_t* data, size_t len);
