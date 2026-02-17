// Consolidated tests for `convertModbusValueToRaw` using the proper API
#/********************************************************************************
 * @file test_modbus_value_to_raw.cpp
 * @brief Tests converting floating-point values into Modbus register words.
 *
 * Goal: Ensure round-trip conversion and correct endian/padding behavior when
 *       preparing values for Modbus writes, from a user's perspective.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <vector>
#include <cstdint>
#include "../../lib/helpers/include/ModbusDevice_helper_portable.h"
// include implementation for native tests
#include "../../lib/helpers/src/ModbusDevice_helper_portable.cpp"

using namespace ModbusDeviceHelper;

/**
 * @brief UINT16 conversion to raw register words.
 * @goal Ensure a large value fits into a single UINT16 register when encoded.
 */
void test_uint16_conversion() {
    ModbusRegisterDef def{};
    def.dataType = ModbusDataType::UINT16;
    def.length = 1;
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    auto r = convertModbusValueToRaw(def, 40000.0f);
    TEST_ASSERT_EQUAL_UINT32(1u, r.size());
    TEST_ASSERT_EQUAL_UINT16(40000u, r[0]);
}

/**
 * @brief UINT32 endian encoding tests.
 * @goal Verify that 32-bit integers are split into two 16-bit registers in
 *       the correct endian order for BE and LE modes.
 */
void test_uint32_endian()
{
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;
    def.length = 2;

    def.dataType = ModbusDataType::UINT32_BE;
    auto rbe = convertModbusValueToRaw(def, 0x01020304);
    TEST_ASSERT_EQUAL_UINT32(2u, rbe.size());
    TEST_ASSERT_EQUAL_UINT16(0x0102u, rbe[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0304u, rbe[1]);

    def.dataType = ModbusDataType::UINT32_LE;
    auto rle = convertModbusValueToRaw(def, 0x01020304);
    TEST_ASSERT_EQUAL_UINT32(2u, rle.size());
    TEST_ASSERT_EQUAL_UINT16(0x0304u, rle[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0102u, rle[1]);
}

/**
 * @brief FLOAT32 roundtrip and endian tests.
 * @goal Confirm float32 values round-trip through conversion and respect
 *       endian ordering for both BE and LE.
 */
void test_float32_endian_and_roundtrip()
{
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;
    def.length = 2;

    def.dataType = ModbusDataType::FLOAT32_BE;
    float fv = 1.5f;
    auto rbe = convertModbusValueToRaw(def, fv);
    TEST_ASSERT_EQUAL_UINT32(2u, rbe.size());
    float out = convertModbusRawToValue(def, rbe.data());
    TEST_ASSERT_FLOAT_WITHIN(1e-6, fv, out);

    def.dataType = ModbusDataType::FLOAT32_LE;
    auto rle = convertModbusValueToRaw(def, fv);
    TEST_ASSERT_EQUAL_UINT32(2u, rle.size());
    out = convertModbusRawToValue(def, rle.data());
    TEST_ASSERT_FLOAT_WITHIN(1e-6, fv, out);
}

/**
 * @brief Boolean encoding and padding behavior.
 * @goal Ensure booleans map to 0/1 and values are padded to the requested
 *       register count with zeros.
 */
void test_bool_and_padding()
{
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::BOOL;
    def.length = 1;
    auto rb = convertModbusValueToRaw(def, 0.0f);
    TEST_ASSERT_EQUAL_UINT32(1u, rb.size());
    TEST_ASSERT_EQUAL_UINT16(0u, rb[0]);
    auto rb1 = convertModbusValueToRaw(def, 2.0f);
    TEST_ASSERT_EQUAL_UINT16(1u, rb1[0]);

    def.dataType = ModbusDataType::UINT16;
    def.length = 3; // padding longer than needed
    auto rp = convertModbusValueToRaw(def, 1.0f);
    TEST_ASSERT_EQUAL_UINT32(3u, rp.size());
    TEST_ASSERT_EQUAL_UINT16(1u, rp[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, rp[1]);
    TEST_ASSERT_EQUAL_UINT16(0u, rp[2]);
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_uint16_conversion);
    RUN_TEST(test_uint32_endian);
    RUN_TEST(test_float32_endian_and_roundtrip);
    RUN_TEST(test_bool_and_padding);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_uint16_conversion);
    RUN_TEST(test_uint32_endian);
    RUN_TEST(test_float32_endian_and_roundtrip);
    RUN_TEST(test_bool_and_padding);
    return UNITY_END();
}
#endif
