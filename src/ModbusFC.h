#ifndef MODBUS_FC_H
#define MODBUS_FC_H

#include <cstdint>

/**
 * @file ModbusFC.h
 * @brief Modbus function code constants
 *
 * Defines named constants for common Modbus RTU function codes used across
 * the firmware. Placed in a small namespace to avoid polluting the global
 * namespace while remaining usable in constexpr contexts.
 */
namespace ModbusFC {
    constexpr uint8_t READ_COILS = 0x01;                  /**< Read Coils (0x01) */
    constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;         /**< Read Discrete Inputs (0x02) */
    constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;      /**< Read Holding Registers (0x03) */
    constexpr uint8_t READ_INPUT_REGISTERS = 0x04;        /**< Read Input Registers (0x04) */
    constexpr uint8_t WRITE_SINGLE_COIL = 0x05;           /**< Write Single Coil (0x05) */
    constexpr uint8_t WRITE_SINGLE_REGISTER = 0x06;       /**< Write Single Register (0x06) */
    constexpr uint8_t WRITE_MULTIPLE_COILS = 0x0F;        /**< Write Multiple Coils (0x0F) */
    constexpr uint8_t WRITE_MULTIPLE_REGISTERS = 0x10;    /**< Write Multiple Registers (0x10) */
}

#endif // MODBUS_FC_H