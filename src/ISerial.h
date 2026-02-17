#pragma once
#include <cstdint>
#include <cstddef>

/**
 * @brief Serial interface abstraction used by the Modbus layer
 *
 * `ISerial` provides a minimal subset of the Arduino `HardwareSerial`
 * functionality required by the transport layer. 
 * Needed for mockup/unit testing of Modbus logic without hardware
 */
struct ISerial {
    /**
     * @brief Initialize the serial port
     * @param baud Baud rate in bits per second
     * @param config Optional serial configuration flags
     * @param rx Optional RX pin (board-specific)
     * @param tx Optional TX pin (board-specific)
     */
    virtual void begin(uint32_t baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1) = 0;

    /**
     * @brief Number of bytes available for reading
     * @return Number of bytes available
     */
    virtual int available() = 0;

    /**
     * @brief Read a single byte (or -1 if none available)
     * @return Byte value or -1
     */
    virtual int read() = 0;

    /**
     * @brief Write a single byte
     * @param b Byte to write
     * @return Number of bytes written (1 on success)
     */
    virtual size_t write(uint8_t b) = 0;

    /**
     * @brief Flush the transmit buffer
     */
    virtual void flush() = 0;

    virtual ~ISerial() = default;
};
