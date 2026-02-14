// Consolidated tests for `convertModbusValueToRaw` using the proper API
#include <unity.h>
#include <vector>
#include <cstdint>
#include "../../lib/helpers/include/ModbusDevice_helper_portable.h"
// include implementation for native tests
#include "../../lib/helpers/src/ModbusDevice_helper_portable.cpp"

using namespace ModbusDeviceHelper;

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

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_uint16_conversion);
    RUN_TEST(test_uint32_endian);
    RUN_TEST(test_float32_endian_and_roundtrip);
    RUN_TEST(test_bool_and_padding);
    return UNITY_END();
}
