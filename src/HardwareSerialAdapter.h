#pragma once
#include "ISerial.h"
#include <HardwareSerial.h>

/**
 * @brief Adapter implementing `ISerial` using Arduino `HardwareSerial`
 *
 * This small wrapper presents the `HardwareSerial` API behind the
 * `ISerial` virtual interface used by the codebase, enabling test doubles
 * and configurable serial backends.
 */
struct HardwareSerialAdapter : public ISerial {
    HardwareSerial& s;

    /**
     * @brief Construct adapter from a `HardwareSerial` instance
     * @param serial Reference to an existing `HardwareSerial` (e.g., Serial)
     */
    HardwareSerialAdapter(HardwareSerial& serial) : s(serial) {}

    /**
     * @brief Initialize the underlying `HardwareSerial`
     * @param baud Baud rate
     * @param config Optional serial configuration flags
     * @param rx Optional RX pin (board-dependent)
     * @param tx Optional TX pin (board-dependent)
     */
    void begin(uint32_t baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1) override {
        if (rx >= 0 && tx >= 0) s.begin(baud, config, rx, tx);
        else s.begin(baud, config);
    }

    int available() override { return s.available(); }
    int read() override { return s.read(); }
    size_t write(uint8_t b) override { return s.write(b); }
    void flush() override { s.flush(); }
};
