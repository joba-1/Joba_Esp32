#/********************************************************************************
 * @file test_modbus_rtu_determine_length_more.cpp
 * @brief More tests for determining Modbus RTU frame lengths.
 *
 * Goal: Ensure frame-length heuristics correctly identify requests, responses,
 *       and exception frames so scheduling and parsing remain reliable.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <vector>
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

/**
 * @brief Too-short buffer handling.
 * @goal Ensure frame-length detection rejects undersized buffers.
 */
void test_short_buffer_returns_false(void) {
    // Arrange
    uint8_t buf[3] = {0x01, 0x03, 0x00};
    bool isReq = false; size_t len = 0;
    // Act & Assert
    TEST_ASSERT_FALSE(ModbusRTUHelper::determineFrameLength(buf, 3, isReq, len));
}

/**
 * @brief Exception response length detection.
 * @goal Validate that exception responses report the correct total length.
 */
void test_exception_response_length(void) {
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

/**
 * @brief Read request and response length detection.
 * @goal Confirm determineFrameLength reports lengths for both request and
 *       corresponding response forms.
 */
void test_read_request_and_response_lengths(void) {
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
