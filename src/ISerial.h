#pragma once
#include <cstdint>
#include <cstddef>

struct ISerial {
    virtual void begin(uint32_t baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1) = 0;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual size_t write(uint8_t b) = 0;
    virtual void flush() = 0;
    virtual ~ISerial() = default;
};
