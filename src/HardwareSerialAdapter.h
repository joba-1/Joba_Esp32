#pragma once
#include "ISerial.h"
#include <HardwareSerial.h>

struct HardwareSerialAdapter : public ISerial {
    HardwareSerial& s;
    HardwareSerialAdapter(HardwareSerial& serial) : s(serial) {}
    void begin(uint32_t baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1) override {
        if (rx >= 0 && tx >= 0) s.begin(baud, config, rx, tx);
        else s.begin(baud, config);
    }
    int available() override { return s.available(); }
    int read() override { return s.read(); }
    size_t write(uint8_t b) override { return s.write(b); }
    void flush() override { s.flush(); }
};
