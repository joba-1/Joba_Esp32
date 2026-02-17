#/********************************************************************************
 * @file test_modbus_device_helper_more.cpp
 * @brief Additional tests for Modbus device helper edge cases.
 *
 * Goal: Cover edge behaviors (endianess, padding, boolean semantics) so users
 *       gain confidence the conversion logic handles uncommon inputs.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <cstring>
#include "../../lib/helpers/include/ModbusDevice_helper_portable.h"
// include implementation for native tests
#include "../../lib/helpers/src/ModbusDevice_helper_portable.cpp"

using namespace ModbusDeviceHelper;

/**
 * @brief UINT32 BE/LE interpretation tests.
 * @goal Verify that 32-bit unsigned integers are reconstructed correctly
 *       for both endian conventions.
 */
void test_uint32_be_and_le() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    // Arrange (big-endian uint32)
    // Rationale: pick small numeric sequences (0x0001,0x0002) so the
    // reconstructed integer values are easy to calculate and verify by hand.
    def.dataType = ModbusDataType::UINT32_BE;
    uint16_t raw_be[] = {0x0001, 0x0002}; // 0x00010002 = 65538
    // Act
    float out_be = convertModbusRawToValue(def, raw_be);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 65538.0f, out_be);

    // Arrange (little-endian uint32)
    // Rationale: reuse the same raw words to emphasize how endianess affects
    // interpretation without introducing different numeric patterns.
    def.dataType = ModbusDataType::UINT32_LE;
    uint16_t raw_le[] = {0x0001, 0x0002}; // LE => 0x00020001 = 131073
    // Act
    float out_le = convertModbusRawToValue(def, raw_le);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 131073.0f, out_le);
}

/**
 * @brief INT32 signed behaviour.
 * @goal Ensure signed 32-bit values are interpreted as negative when the
 *       high bit is set, for both endiannesses.
 */
void test_int32_signed_behaviour() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::INT32_BE;
    // Rationale: use all-ones words to represent -1 in two's complement
    // and verify negative value handling for signed 32-bit integers.
    uint16_t raw_neg[] = {0xFFFF, 0xFFFF}; // 0xFFFFFFFF == -1 as int32
    // Arrange & Act
    float v = convertModbusRawToValue(def, raw_neg);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.0f, v);

    def.dataType = ModbusDataType::INT32_LE;
    uint16_t raw_neg_le[] = {0xFFFF, 0xFFFF};
    // Act
    float v2 = convertModbusRawToValue(def, raw_neg_le);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.0f, v2);
}

/**
 * @brief FLOAT32 little-endian reconstruction and scaling.
 * @goal Confirm float32 values reconstruct correctly and conversionFactor/
 *       offset are applied as expected.
 */
void test_float32_le_and_conversion() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::FLOAT32_LE;
    // Rationale: choose 2.5f (small decimal) to make the reconstructed
    // float easy to check while still exercising the IEEE-754 path.
    // 2.5f as bits
    float fv = 2.5f;
    uint32_t bits;
    memcpy(&bits, &fv, sizeof(bits));
    uint16_t raw_le[] = { (uint16_t)(bits & 0xFFFF), (uint16_t)(bits >> 16) };
    // Arrange
    // Act
    float out = convertModbusRawToValue(def, raw_le);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6, fv, out);

    // test conversion factor and offset
    // Rationale: choose conversionFactor=0.1 and offset=1.0 so that
    // raw value 100 maps to a simple expected result (11.0), making
    // the assertion straightforward.
    def.dataType = ModbusDataType::UINT16;
    def.conversionFactor = 0.1f;
    def.offset = 1.0f;
    uint16_t raw[] = { 100 };
    // (100 * 0.1) + 1 = 11
    // Act
    float scaled = convertModbusRawToValue(def, raw);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 11.0f, scaled);
}

/**
 * @brief Default and boolean fallback semantics.
 * @goal Validate default handling (fallback to uint16 behavior) and boolean
 *       truthiness mapping for non-zero values.
 */
void test_default_and_bool() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::STRING; // not specially handled -> default
    uint16_t raw[] = { 42 };
    // Act & Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 42.0f, convertModbusRawToValue(def, raw));

    def.dataType = ModbusDataType::BOOL;
    uint16_t r0[] = {0};
    uint16_t r1[] = {2};
    // Act & Assert
    TEST_ASSERT_EQUAL_FLOAT(0.0f, convertModbusRawToValue(def, r0));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, convertModbusRawToValue(def, r1));
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_uint32_be_and_le);
    RUN_TEST(test_int32_signed_behaviour);
    RUN_TEST(test_float32_le_and_conversion);
    RUN_TEST(test_default_and_bool);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_uint32_be_and_le);
    RUN_TEST(test_int32_signed_behaviour);
    RUN_TEST(test_float32_le_and_conversion);
    RUN_TEST(test_default_and_bool);
    return UNITY_END();
}
#endif
