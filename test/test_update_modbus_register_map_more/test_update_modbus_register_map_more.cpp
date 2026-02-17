#/********************************************************************************
 * @file test_update_modbus_register_map_more.cpp
 * @brief Tests for updating the Modbus register map from request/response pairs.
 *
 * Goal: Verify that register map updates correctly record values and timestamps
 *       so tools interpreting the map present accurate device state to users.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <vector>
#include <map>
#include "../../src/ModbusRegisterMap.h"
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

static std::vector<uint8_t> with_crc(const std::vector<uint8_t>& v) {
    std::vector<uint8_t> out = v;
    uint16_t crc = modbus_crc16(v.data(), v.size());
    out.push_back((uint8_t)(crc & 0xFF));
    out.push_back((uint8_t)((crc >> 8) & 0xFF));
    return out;
}

void test_update_holding_registers(void) {
    // Arrange: request for 2 holding registers at 0x0010
    std::vector<uint8_t> req = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x00, 0x10, 0x00, 0x02};
    auto req_full = with_crc(req);
    std::vector<uint8_t> resp = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x04, 0x00, 0x11, 0x00, 0x22};
    auto resp_full = with_crc(resp);

    ModbusFrame reqFrame;
    ModbusFrame respFrame;
    // Act: parse frames
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(req_full.data(), req_full.size(), reqFrame, 0, 0));
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(resp_full.data(), resp_full.size(), respFrame, 0, 0));

    ModbusRegisterMap regMap{};
    regMap.unitId = reqFrame.unitId;
    regMap.functionCode = reqFrame.functionCode;

    // Act: update map
    ModbusRTUHelper::updateModbusRegisterMap(regMap, reqFrame, respFrame, 12345);

    // Assert
    TEST_ASSERT_EQUAL_UINT32(1u, regMap.responseCount);
    TEST_ASSERT_EQUAL_UINT32(12345u, (uint32_t)regMap.lastUpdate);
    TEST_ASSERT_EQUAL_HEX16(0x0011, regMap.registers[0x0010]);
    TEST_ASSERT_EQUAL_HEX16(0x0022, regMap.registers[0x0011]);
}

void test_update_coils(void) {
    // Arrange: request read coils starting at 0x0020 qty 8
    std::vector<uint8_t> req = {0x01, ModbusFC::READ_COILS, 0x00, 0x20, 0x00, 0x08};
    auto req_full = with_crc(req);
    // Response: bytecount=1, data=0x01 -> first coil true, others false
    std::vector<uint8_t> resp = {0x01, ModbusFC::READ_COILS, 0x01, 0x01};
    auto resp_full = with_crc(resp);

    ModbusFrame reqFrame;
    ModbusFrame respFrame;
    // Act: parse frames
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(req_full.data(), req_full.size(), reqFrame, 0, 0));
    TEST_ASSERT_TRUE(ModbusRTUHelper::parseModbusFrame(resp_full.data(), resp_full.size(), respFrame, 0, 0));

    ModbusRegisterMap coilMap{};
    coilMap.unitId = reqFrame.unitId;
    coilMap.functionCode = reqFrame.functionCode;

    // Act: update map
    ModbusRTUHelper::updateModbusRegisterMap(coilMap, reqFrame, respFrame, 54321);

    // Assert
    TEST_ASSERT_EQUAL_UINT32(1u, coilMap.responseCount);
    TEST_ASSERT_EQUAL_UINT32(54321u, (uint32_t)coilMap.lastUpdate);
    TEST_ASSERT_EQUAL_HEX16(1, coilMap.registers[0x0020]); // first coil true
    TEST_ASSERT_EQUAL_HEX16(0, coilMap.registers[0x0021]); // next coil false
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_update_holding_registers);
    RUN_TEST(test_update_coils);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_update_holding_registers);
    RUN_TEST(test_update_coils);
    return UNITY_END();
}
#endif
