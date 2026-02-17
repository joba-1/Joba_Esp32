#/********************************************************************************
 * @file test_modbus_helpers.cpp
 * @brief Unit tests for Modbus helper utilities (CRC and hex formatting).
 *
 * Goal: From a user perspective, ensure low-level Modbus utilities produce
 *       correct CRCs and human-friendly hex formatting for diagnostics.
 */

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

/**
 * @brief CRC residual test when CRC bytes are appended to data.
 * @goal Ensure the CRC implementation returns a zero residual when the
 *       correct CRC is appended to the data stream.
 */
static void test_crc_with_appended_bytes_should_verify_zero_residual(void) {
    // Arrange
    // Use a short, typical Modbus read request fragment: unit=1, fc=3,
    // two data bytes (0x00,0x0A). This small, deterministic payload is
    // easy to verify and produces a known CRC for the test.
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

/**
 * @brief Hex formatting correctness.
 * @goal Verify that `format_hex` emits uppercase hex pairs separated by spaces.
 */
static void test_format_hex_basic(void) {
    // Arrange
    // A short byte sequence chosen to exercise formatting: includes a
    // single-byte low values and a high byte (0xAB) to verify hex width
    // and spacing in the output.
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
