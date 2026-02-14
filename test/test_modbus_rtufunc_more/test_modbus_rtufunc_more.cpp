#include <unity.h>

// Include implementations so they're compiled into the test binary
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

void test_modbusframe_getters(void) {
    ModbusFrame f;
    f.dataLen = 0;
    TEST_ASSERT_EQUAL_UINT16(0, f.getStartRegister());
    TEST_ASSERT_EQUAL_UINT16(0, f.getQuantity());
    TEST_ASSERT_EQUAL_UINT32(0, f.getByteCount());
    TEST_ASSERT_NULL(f.getRegisterData());

    // set minimal data for start register
    f.dataLen = 2;
    f.data[0] = 0x01;
    f.data[1] = 0x02;
    TEST_ASSERT_EQUAL_UINT16(0x0102, f.getStartRegister());

    // set data for quantity
    f.dataLen = 4;
    f.data[2] = 0x00;
    f.data[3] = 0x03;
    TEST_ASSERT_EQUAL_UINT16(0x0003, f.getQuantity());
}

void test_determine_frame_length_request(void) {
    // request: unit(1), fc(3), start(0), qty(2) -> 6 bytes without CRC
    uint8_t msg[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    uint16_t crc = modbus_crc16(msg, 6);
    msg[6] = crc & 0xFF;
    msg[7] = (crc >> 8) & 0xFF;

    bool isReq = false;
    size_t frameLen = 0;
    bool ok = ModbusRTUHelper::determineFrameLength(msg, sizeof(msg), isReq, frameLen);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(isReq);
    TEST_ASSERT_EQUAL_UINT(8, frameLen);
}

void test_update_register_map_holding(void) {
    // build request (start=0, qty=2)
    uint8_t req[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    uint16_t crc1 = modbus_crc16(req, 6);
    req[6] = crc1 & 0xFF; req[7] = (crc1 >> 8) & 0xFF;

    // build response: unit, fc, bytecount=4, data (0x000A,0x0014)
    uint8_t resp[9] = { 0x01, 0x03, 0x04, 0x00, 0x0A, 0x00, 0x14, 0x00, 0x00 };
    uint16_t crc2 = modbus_crc16(resp, 7);
    resp[7] = crc2 & 0xFF; resp[8] = (crc2 >> 8) & 0xFF;

    ModbusFrame request;
    ModbusFrame response;

    ModbusRTUHelper::parseModbusFrame(req, sizeof(req), request, 0, 0);
    ModbusRTUHelper::parseModbusFrame(resp, sizeof(resp), response, 0, 0);

    ModbusRegisterMap map{};
    map.unitId = 1;
    map.functionCode = 3;
    map.responseCount = 0;
    map.lastUpdate = 0;

    ModbusRTUHelper::updateModbusRegisterMap(map, request, response, 12345);

    // expect two registers updated
    TEST_ASSERT_EQUAL_UINT16(10, map.registers[0]);
    TEST_ASSERT_EQUAL_UINT16(20, map.registers[1]);
    TEST_ASSERT_EQUAL_UINT32(1, map.responseCount);
    TEST_ASSERT_EQUAL_UINT32(12345, map.lastUpdate);
}

void setUp(void) {}
void tearDown(void) {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_modbusframe_getters);
    RUN_TEST(test_determine_frame_length_request);
    RUN_TEST(test_update_register_map_holding);
    UNITY_END();
    return 0;
}
