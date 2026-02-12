#include <unity.h>
#include "modbus_helpers.h"
// For native tests we compile the helper implementation into the test TU
// to avoid building the whole Arduino-based firmware tree.
#include "../../src/modbus_helpers.cpp"
#include <vector>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

void setUp(void) {}
void tearDown(void) {}

static void test_crc_with_appended_bytes_should_verify_zero_residual(void) {
    // Arrange
    uint8_t data[] = {0x01, 0x03, 0x02, 0x00, 0x0A};
    size_t len = sizeof(data)/sizeof(data[0]);

    // Act
    uint16_t crc = modbus_crc16(data, len);
    std::vector<uint8_t> withCrc(data, data + len);
    withCrc.push_back((uint8_t)(crc & 0xFF));      // CRC low
    withCrc.push_back((uint8_t)((crc >> 8) & 0xFF)); // CRC high
    uint16_t residual = modbus_crc16(withCrc.data(), withCrc.size());

    // Assert
    TEST_ASSERT_EQUAL_HEX16(0x0000, residual);
}

static void test_format_hex_basic(void) {
    // Arrange
    uint8_t data[] = {0x01, 0x02, 0xAB};

    // Act
    std::string s = format_hex(data, sizeof(data));

    // Assert
    TEST_ASSERT_EQUAL_STRING("01 02 AB", s.c_str());
}

// On Arduino targets (serial test runner) Unity uses `setup`/`loop` instead of main().
#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_crc_with_appended_bytes_should_verify_zero_residual);
    RUN_TEST(test_format_hex_basic);
}

void loop() {
    UNITY_END();
    while (true) {
        delay(1000);
    }
}
#else
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_with_appended_bytes_should_verify_zero_residual);
    RUN_TEST(test_format_hex_basic);
    return UNITY_END();
}
#endif
