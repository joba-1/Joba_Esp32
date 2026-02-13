#ifndef MODBUS_REGISTER_MAP_H
#define MODBUS_REGISTER_MAP_H

#include <cstdint>
#include <map>

struct ModbusRegisterMap {
    uint8_t unitId;
    uint8_t functionCode;
    std::map<uint16_t, uint16_t> registers;  // address -> value
    unsigned long lastUpdate;
    uint32_t requestCount;
    uint32_t responseCount;
    uint32_t errorCount;
};

#endif // MODBUS_REGISTER_MAP_H