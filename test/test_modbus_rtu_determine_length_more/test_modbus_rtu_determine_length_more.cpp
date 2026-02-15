#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <vector>
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

void test_short_buffer_returns_false() {
    // Arrange
    uint8_t buf[3] = {0x01, 0x03, 0x00};
    bool isReq = false; size_t len = 0;
    // Act & Assert
    TEST_ASSERT_FALSE(ModbusRTUHelper::determineFrameLength(buf, 3, isReq, len));
}

void test_exception_response_length() {
    // Arrange: exception response (unit, fc|0x80, exCode, CRC appended)
    std::vector<uint8_t> raw = {0x11, (uint8_t)(0x03 | 0x80), 0x05};
    uint16_t crc = modbus_crc16(raw.data(), raw.size());
    raw.push_back((uint8_t)(crc & 0xFF));
    raw.push_back((uint8_t)((crc >> 8) & 0xFF));
    bool isReq = true; size_t len = 0;
    // Act
    bool ok = ModbusRTUHelper::determineFrameLength(raw.data(), raw.size(), isReq, len);
    // Assert
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(isReq);
    TEST_ASSERT_EQUAL_UINT(5, len);
}

void test_read_request_and_response_lengths() {
    // Arrange: request for 2 registers (unit, fc, startHi, startLo, qtyHi, qtyLo, crcLo, crcHi)
    uint8_t req[] = {0x01, 0x03, 0x00, 0x10, 0x00, 0x02, 0x00, 0x00};
    bool isReq = false; size_t len = 0;
    // Act
    TEST_ASSERT_TRUE(ModbusRTUHelper::determineFrameLength(req, sizeof(req), isReq, len));
    // Assert
    TEST_ASSERT_TRUE(isReq);
    TEST_ASSERT_EQUAL_UINT(8, len);

    // Arrange: response with bytecount=4 => total 1+1+1+4+2 = 9
    uint8_t resp[] = {0x01, 0x03, 0x04, 0x00,0x11,0x00,0x22, 0x00,0x00};
    isReq = false; len = 0;
    // Act
    TEST_ASSERT_TRUE(ModbusRTUHelper::determineFrameLength(resp, sizeof(resp), isReq, len));
    // Assert
    TEST_ASSERT_FALSE(isReq);
    TEST_ASSERT_EQUAL_UINT(9, len);
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_short_buffer_returns_false);
    RUN_TEST(test_exception_response_length);
    RUN_TEST(test_read_request_and_response_lengths);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_short_buffer_returns_false);
    RUN_TEST(test_exception_response_length);
    RUN_TEST(test_read_request_and_response_lengths);
    return UNITY_END();
}
#endif
