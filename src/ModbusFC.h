#ifndef MODBUS_FC_H
#define MODBUS_FC_H

#include <cstdint>

namespace ModbusFC {
    constexpr uint8_t READ_COILS = 0x01;
    constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;
    constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;
    constexpr uint8_t READ_INPUT_REGISTERS = 0x04;
    constexpr uint8_t WRITE_SINGLE_COIL = 0x05;
    constexpr uint8_t WRITE_SINGLE_REGISTER = 0x06;
    constexpr uint8_t WRITE_MULTIPLE_COILS = 0x0F;
    constexpr uint8_t WRITE_MULTIPLE_REGISTERS = 0x10;
}

#endif // MODBUS_FC_H