#ifndef MODBUS_REGISTER_MAP_H
#define MODBUS_REGISTER_MAP_H

#include <cstdint>
#include <map>

/**
 * @brief Simple container summarizing observed registers for a unit/FC
 *
 * Used by the bus-monitoring logic to track detected register values for a
 * specific `unitId` and `functionCode`. The `registers` map stores the last
 * seen value per address and the counters/statistics support diagnostics.
 */
struct ModbusRegisterMap {
    uint8_t unitId;                            /**< Modbus unit id */
    uint8_t functionCode;                      /**< Modbus function code */
    std::map<uint16_t, uint16_t> registers;    /**< address -> last seen value */
    unsigned long lastUpdate;                  /**< millis() of last update */
    uint32_t requestCount;                     /**< number of requests observed */
    uint32_t responseCount;                    /**< number of responses observed */
    uint32_t errorCount;                       /**< number of errors observed */
};

#endif // MODBUS_REGISTER_MAP_H