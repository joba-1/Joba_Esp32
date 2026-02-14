#include <unity.h>
#include <cstring>
#include "../../lib/helpers/include/ModbusDevice_helper_portable.h"
// include implementation for native tests
#include "../../lib/helpers/src/ModbusDevice_helper_portable.cpp"

using namespace ModbusDeviceHelper;

void test_uint32_be_and_le() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    // Arrange (big-endian uint32)
    def.dataType = ModbusDataType::UINT32_BE;
    uint16_t raw_be[] = {0x0001, 0x0002}; // 0x00010002 = 65538
    // Act
    float out_be = convertModbusRawToValue(def, raw_be);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 65538.0f, out_be);

    // Arrange (little-endian uint32)
    def.dataType = ModbusDataType::UINT32_LE;
    uint16_t raw_le[] = {0x0001, 0x0002}; // LE => 0x00020001 = 131073
    // Act
    float out_le = convertModbusRawToValue(def, raw_le);
    // Assert
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 131073.0f, out_le);
}

void test_int32_signed_behaviour() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::INT32_BE;
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

void test_float32_le_and_conversion() {
    ModbusRegisterDef def{};
    def.conversionFactor = 1.0f;
    def.offset = 0.0f;

    def.dataType = ModbusDataType::FLOAT32_LE;
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_uint32_be_and_le);
    RUN_TEST(test_int32_signed_behaviour);
    RUN_TEST(test_float32_le_and_conversion);
    RUN_TEST(test_default_and_bool);
    return UNITY_END();
}
