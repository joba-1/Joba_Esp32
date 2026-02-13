#pragma once
#include <stdint.h>
#include <string>

/**
 * @file modbus_helpers.h
 * @brief Small helper utilities used across the Modbus stack
 */

/**
 * @brief Calculate Modbus RTU CRC-16 (polynomial 0xA001)
 *
 * Standard CRC used by Modbus RTU frames. The function computes the CRC for
 * the provided buffer and returns the 16-bit CRC value in little-endian
 * ordering (LSB first) as commonly appended to Modbus frames.
 *
 * @param data Pointer to the input buffer
 * @param len  Number of bytes in the buffer
 * @return 16-bit CRC value
 */
uint16_t modbus_crc16(const uint8_t* data, size_t len);

/**
 * @brief Format a byte buffer as hex bytes separated by spaces
 *
 * Produces a human-readable ASCII string such as "01 02 AB" used for
 * logging Modbus frames.
 *
 * @param data Pointer to the input buffer
 * @param len  Number of bytes in the buffer
 * @return std::string with formatted hex bytes
 */
std::string format_hex(const uint8_t* data, size_t len);
