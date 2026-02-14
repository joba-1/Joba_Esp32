#ifndef MODBUS_FRAME_H
#define MODBUS_FRAME_H

#include <array>
#include <cstdint>
#include <cstddef>

/**
 * @brief A single Modbus RTU frame (request or response)
 *
 * Represents a parsed Modbus RTU frame captured from the bus. `data` contains
 * the payload (excluding unit id, function code and CRC). `timestamp` records
 * `millis()` at capture time (monotonic) and `unixTimestamp` stores epoch
 * seconds when available. Helper accessors extract common fields used by
 * pattern analysis and transaction matching.
 */
struct ModbusFrame {
    static constexpr size_t MAX_DATA_LEN = 252;

    uint8_t unitId;
    uint8_t functionCode;
    std::array<uint8_t, MAX_DATA_LEN> data{};
    uint16_t dataLen{0};
    uint16_t crc;
    unsigned long timestamp;         // millis() at capture time (monotonic)
    uint32_t unixTimestamp;          // epoch seconds at capture time (0 if time invalid)
    bool isRequest;                  // request vs response (best-effort)
    bool isValid;                   // CRC check passed
    bool isException;               // Exception response (FC | 0x80)
    uint8_t exceptionCode;

    /**
     * @brief Extract start register from request payload
     * @return 16-bit start register or 0 when not available
     */
    uint16_t getStartRegister() const {
        if (dataLen >= 2) return (data[0] << 8) | data[1];
        return 0;
    }

    /**
     * @brief Extract quantity field from request payload
     * @return 16-bit quantity or 0 when not available
     */
    uint16_t getQuantity() const {
        if (dataLen >= 4) return (data[2] << 8) | data[3];
        return 0;
    }

    /**
     * @brief Get byte-count from a register read response payload
     * @return number of payload bytes (not including the byte-count field)
     */
    size_t getByteCount() const {
        if (dataLen >= 1) return data[0];
        return 0;
    }

    /**
     * @brief Return pointer to the register payload (after the byte-count)
     * @return pointer to payload or nullptr when not present
     */
    const uint8_t* getRegisterData() const {
        if (dataLen > 1) return &data[1];
        return nullptr;
    }
};

#endif // MODBUS_FRAME_H
