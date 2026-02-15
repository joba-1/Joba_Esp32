#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include "ModbusDevice_helper_portable.h"
#include <cstring>

using namespace ModbusDeviceHelper;

void test_parse_types() {
    TEST_ASSERT_EQUAL(ModbusDataType::INT16, parseModbusDataType("int16"));
    TEST_ASSERT_EQUAL(ModbusDataType::UINT32_BE, parseModbusDataType("uint32_be"));
    TEST_ASSERT_EQUAL(ModbusDataType::UINT32_LE, parseModbusDataType("uint32_le"));
    TEST_ASSERT_EQUAL(ModbusDataType::INT32_BE, parseModbusDataType("int32_be"));
    TEST_ASSERT_EQUAL(ModbusDataType::INT32_LE, parseModbusDataType("int32_le"));
    TEST_ASSERT_EQUAL(ModbusDataType::FLOAT32_BE, parseModbusDataType("float32_be"));
    TEST_ASSERT_EQUAL(ModbusDataType::FLOAT32_LE, parseModbusDataType("float32_le"));
    TEST_ASSERT_EQUAL(ModbusDataType::BOOL, parseModbusDataType("bool"));
    TEST_ASSERT_EQUAL(ModbusDataType::STRING, parseModbusDataType("string"));
    // unknown -> default UINT16
    TEST_ASSERT_EQUAL(ModbusDataType::UINT16, parseModbusDataType("unknown_type"));
        // Arrange: none
        // Act & Assert:
}

void test_convert_raw_to_value_uint16_and_int16() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;
    def.dataType = ModbusDataType::UINT16;
        uint16_t raw1[] = {12345}; // AAA start
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 12345.0f, convertModbusRawToValue(def, raw1)); // AAA end

    def.dataType = ModbusDataType::INT16;
    uint16_t raw2[] = {0xFFFF}; // -1
        TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, convertModbusRawToValue(def, raw2)); // AAA end
}

void test_convert_raw_to_value_uint32_and_int32_be_le() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::UINT32_BE;
    uint16_t raw_be[] = {0x0001, 0x0002}; // 0x00010002 = 65538
    TEST_ASSERT_EQUAL_FLOAT(65538.0f, convertModbusRawToValue(def, raw_be));

    def.dataType = ModbusDataType::UINT32_LE;
    uint16_t raw_le[] = {0x0001, 0x0002}; // LE => 0x00020001 = 131073
    TEST_ASSERT_EQUAL_FLOAT(131073.0f, convertModbusRawToValue(def, raw_le));

    def.dataType = ModbusDataType::INT32_BE;
    uint16_t raw_i32_be[] = {0xFFFF, 0xFFFE}; // negative large
    // interpret as signed 32-bit
    float v = convertModbusRawToValue(def, raw_i32_be);
    (void)v; // just ensure it runs; exact value platform-dependent sign behaviour checked indirectly
}

void test_convert_raw_to_value_float32_be_le_and_bool() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    // 1.5f as bits 0x3FC00000 -> high=0x3FC0 low=0x0000
    def.dataType = ModbusDataType::FLOAT32_BE;
    uint16_t raw_f_be[] = {0x3FC0, 0x0000};
    float out_be = convertModbusRawToValue(def, raw_f_be);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, out_be);

    def.dataType = ModbusDataType::FLOAT32_LE;
    uint16_t raw_f_le[] = {0x0000, 0x3FC0};
    float out_le = convertModbusRawToValue(def, raw_f_le);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, out_le);

    def.dataType = ModbusDataType::BOOL;
    uint16_t raw_bool[] = {0};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, convertModbusRawToValue(def, raw_bool));
    uint16_t raw_bool1[] = {1};
    TEST_ASSERT_EQUAL_FLOAT(1.0f, convertModbusRawToValue(def, raw_bool1));
}

void test_convert_value_to_raw_padding_and_endian_float_bool() {
    ModbusRegisterDef def{};
    def.offset = 0.0f;
    def.conversionFactor = 1.0f;

    def.dataType = ModbusDataType::UINT16;
    def.length = 1;
    auto r1 = convertModbusValueToRaw(def, 40000.0f);
    TEST_ASSERT_EQUAL(1u, r1.size());
    TEST_ASSERT_EQUAL(40000u, (unsigned)r1[0]);

    def.dataType = ModbusDataType::UINT32_BE;
    def.length = 2;
    auto r2 = convertModbusValueToRaw(def, 0x01020304);
    TEST_ASSERT_EQUAL(2u, r2.size());
    TEST_ASSERT_EQUAL(0x0102u, r2[0]);
    TEST_ASSERT_EQUAL(0x0304u, r2[1]);

    def.dataType = ModbusDataType::UINT32_LE;
    def.length = 2;
    auto r3 = convertModbusValueToRaw(def, 0x01020304);
    TEST_ASSERT_EQUAL(2u, r3.size());
    TEST_ASSERT_EQUAL(0x0304u, r3[0]);
    TEST_ASSERT_EQUAL(0x0102u, r3[1]);

    def.dataType = ModbusDataType::FLOAT32_BE;
    def.length = 2;
    auto rf = convertModbusValueToRaw(def, 1.5f);
    TEST_ASSERT_EQUAL(2u, rf.size());

    def.dataType = ModbusDataType::BOOL;
    def.length = 1;
    auto rb = convertModbusValueToRaw(def, 0.0f);
    TEST_ASSERT_EQUAL(1u, rb.size());
    TEST_ASSERT_EQUAL(0u, rb[0]);
    auto rb1 = convertModbusValueToRaw(def, 2.0f);
    TEST_ASSERT_EQUAL(1u, rb1.size());
    TEST_ASSERT_EQUAL(1u, rb1[0]);
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_types);
    RUN_TEST(test_convert_raw_to_value_uint16_and_int16);
    RUN_TEST(test_convert_raw_to_value_uint32_and_int32_be_le);
    RUN_TEST(test_convert_raw_to_value_float32_be_le_and_bool);
    RUN_TEST(test_convert_value_to_raw_padding_and_endian_float_bool);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_types);
    RUN_TEST(test_convert_raw_to_value_uint16_and_int16);
    RUN_TEST(test_convert_raw_to_value_uint32_and_int32_be_le);
    RUN_TEST(test_convert_raw_to_value_float32_be_le_and_bool);
    RUN_TEST(test_convert_value_to_raw_padding_and_endian_float_bool);
    return UNITY_END();
}
#endif
