#/********************************************************************************
 * @file test_modbus_rtufunc.cpp
 * @brief Tests for Modbus RTU frame parsing and length determination.
 *
 * Goal: Verify the parsing and frame-length heuristics used to separate
 *       requests/responses on the wire so users can rely on robust frame
 *       detection in native test environments.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

// Include the portable helper implementation directly so it is compiled
// into the test binary and coverage is collected.
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

/**
 * @brief Parse a simple Modbus read request.
 * @goal Confirm the frame parser recognises a minimal read request and
 *       reports validity/state correctly (including CRC handling).
 */
void test_parse_simple_read_request(void) {
    // Build a minimal read holding registers request: unit(1), fc(3), start(0), qty(2), crc(2)

    // Arrange
    // Construct a minimal, canonical Modbus "Read Holding Registers"
    // request: unit=1, function=3, start=0, quantity=2. This is the simplest
    // useful request shape and exercises parsing logic for common traffic.
    uint8_t raw[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    ModbusFrame out;
    
    // Act
    bool ok = ModbusRTUHelper::parseModbusFrame(raw, 8, out, 12345, 0);
    
    // Assert
    TEST_ASSERT_TRUE(ok);
    // CRC mismatch expected (we didn't set CRC), parseModbusFrame returns true so caller can log
    TEST_ASSERT_FALSE(out.isValid);
}

/**
 * @brief Determine expected frame length for a response.
 * @goal Ensure `determineFrameLength` returns the correct response length
 *       and identifies the buffer as a response rather than a request.
 */
void test_determine_frame_length_response(void) {
    // Response: unit(1), fc(3), bytecount(4), data(4), crc(2) => total 1+1+1+4+2=9

    // Arrange
    // Build a representative response for two registers: unit=1, fc=3,
    // bytecount=4, two 16-bit register values. This mirrors realistic device
    // responses and allows verifying the computed frame length.
    uint8_t resp[] = { 0x01, 0x03, 0x04, 0x00,0x01,0x00,0x02, 0x00,0x00 };
    bool isReq = false;
    size_t frameLen = 0;

    // Act
    bool ok = ModbusRTUHelper::determineFrameLength(resp, sizeof(resp), isReq, frameLen);
    
    // Assert
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(isReq);
    TEST_ASSERT_EQUAL_UINT(9, frameLen);
}

void setUp(void) {}
void tearDown(void) {}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_simple_read_request);
    RUN_TEST(test_determine_frame_length_response);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_simple_read_request);
    RUN_TEST(test_determine_frame_length_response);
    UNITY_END();
    return 0;
}
#endif
