#/********************************************************************************
 * @file test_modbus_device_helper.cpp
 * @brief Tests for Modbus device helper conversions (raw <-> value).
 *
 * Goal: Validate register data type parsing and numeric conversions so that
 *       consumers can rely on correct value interpretation from device frames.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <cstring>
#include <cstdint>
#include <vector>

// Use the portable helper implementation so tests exercise the real code
#include "../../lib/helpers/include/ModbusDevice_helper_portable.h"
// Include the portable implementation directly into the test binary for native builds
#include "../../lib/helpers/src/ModbusDevice_helper_portable.cpp"

/**
 * @brief Data type parsing behaviour.
 * @goal Ensure textual type names are parsed to the correct `ModbusDataType`
 *       values so users' device JSON types are interpreted correctly.
 */
void test_parse_types(void) {
    using namespace ModbusDeviceHelper;
    // Arrange: none (stateless parser)
    // Rationale: the parser is pure and operates only on the input string;
    // using no setup keeps the test focused on parsing logic and avoids
    // accidental state leakage between cases.
    // Act & Assert
    TEST_ASSERT_TRUE(parseModbusDataType("int16") == ModbusDataType::INT16);
    TEST_ASSERT_TRUE(parseModbusDataType("UINT32_BE") == ModbusDataType::UINT32_BE);
    TEST_ASSERT_TRUE(parseModbusDataType("float32_le") == ModbusDataType::FLOAT32_LE);
}

/**
 * @brief UINT16 / INT16 conversion correctness.
 * @goal Verify raw 16-bit words are converted to signed/unsigned float values
 *       according to the specified `dataType`.
 */
void test_convert_uint16_and_int16(void) {
    ModbusRegisterDef def = {};
    def.dataType = ModbusDataType::UINT16;
    def.length = 1;
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;
    uint16_t raw[] = { 0x1234 };
    // Arrange: def and raw above
    // Rationale: select `0x1234` as a clear, nontrivial 16-bit pattern that
    // is easy to recognize in assertions and not near edge values like 0.
    // Act
    float v = ModbusDeviceHelper::convertModbusRawToValue(def, raw);
    // Assert
    TEST_ASSERT_EQUAL_FLOAT(0x1234, v);

    def.dataType = ModbusDataType::INT16;
    int16_t raw2[] = { (int16_t)0xFF00 };
    float v2 = ModbusDeviceHelper::convertModbusRawToValue(def, (uint16_t*)raw2);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, (float)(int16_t)0xFF00, v2);
}

/**
 * @brief FLOAT32 big-endian conversion.
 * @goal Confirm 32-bit big-endian floats are reconstructed accurately from
 *       two Modbus words within numeric tolerance.
 */
void test_convert_float32_be(void) {
    ModbusRegisterDef def = {};
    def.dataType = ModbusDataType::FLOAT32_BE;
    def.length = 2;
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    float fv = 3.14159f;
    uint32_t bits;
    memcpy(&bits, &fv, sizeof(float));
    uint16_t raw[2];
    raw[0] = (uint16_t)(bits >> 16);
    raw[1] = (uint16_t)(bits & 0xFFFF);
    // Arrange
    // Rationale: use a well-known float `3.14159` to test 32-bit float
    // reconstruction; packing into two 16-bit words mirrors Modbus wire format.
    // Act
    float out = ModbusDeviceHelper::convertModbusRawToValue(def, raw);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6, fv, out);
}

void setUp(void) {}
void tearDown(void) {}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_types);
    RUN_TEST(test_convert_uint16_and_int16);
    RUN_TEST(test_convert_float32_be);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_types);
    RUN_TEST(test_convert_uint16_and_int16);
    RUN_TEST(test_convert_float32_be);
    UNITY_END();
    return 0;
}
#endif
