#include <unity.h>
#include <vector>
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

// Helper to append CRC to a buffer
static std::vector<uint8_t> with_crc(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out = v;
    uint16_t crc = modbus_crc16(v.data(), v.size());
    out.push_back((uint8_t)(crc & 0xFF));
    out.push_back((uint8_t)((crc >> 8) & 0xFF));
    return out;
}

void test_parse_valid_response_and_invalid_crc(void) {
    // Arrange: valid response for 2 registers
    std::vector<uint8_t> resp = {0x01, 0x03, 0x04, 0x00, 0x11, 0x00, 0x22};
    auto full = with_crc(resp);
    ModbusFrame frame;
    // Act
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(full.data(), full.size(), frame, 0, 0));
    // Assert
    TEST_ASSERT_TRUE(frame.isValid);
    TEST_ASSERT_FALSE(frame.isException);
    TEST_ASSERT_EQUAL(4u, frame.getByteCount());

    // Corrupt CRC
    full.back() ^= 0xFF;
    ModbusFrame frame2;
    // Act
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(full.data(), full.size(), frame2, 0, 0));
    // Assert
    TEST_ASSERT_FALSE(frame2.isValid);
}

void test_parse_exception_frame(void) {
    // Arrange: exception frame (unit, fc|0x80, exCode)
    std::vector<uint8_t> exc = {0x11, (uint8_t)(0x03 | 0x80), 0x05};
    auto full = with_crc(exc);
    ModbusFrame frame;
    // Act
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(full.data(), full.size(), frame, 0, 0));
    // Assert
    TEST_ASSERT_TRUE(frame.isValid);
    TEST_ASSERT_TRUE(frame.isException);
    TEST_ASSERT_EQUAL(0x05, frame.exceptionCode);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid_response_and_invalid_crc);
    RUN_TEST(test_parse_exception_frame);
    return UNITY_END();
}
