#/********************************************************************************
 * @file test_modbus_rtufunc_more.cpp
 * @brief Additional Modbus RTU function tests covering parsing and map updates.
 *
 * Goal: Provide broader coverage of RTU function parsing and update logic so
 *       users can trust behavior across varied frame shapes.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

// Include implementations so they're compiled into the test binary
#include "../../src/modbus_helpers.cpp"
#include "../../lib/helpers/src/ModbusRTUFeature_helper.cpp"

/**
 * @brief Accessor helpers on ModbusFrame.
 * @goal Verify getters for start register, quantity, and byte count behave
 *       correctly for different `dataLen` settings.
 */
void test_modbusframe_getters(void) {
    // Arrange:
    // - Start with an empty ModbusFrame (`dataLen=0`) to verify getters
    //   return safe default values when no data is available.
    // - Populate `data` progressively to confirm getters read bytes
    //   from the expected offsets as `dataLen` increases.
    // Rationale: using small, explicit length changes keeps the test focused
    // on accessor correctness and avoids coupling to parsing logic.
    ModbusFrame f;
    f.dataLen = 0;
    
    // Act / Assert: no data available, expect defaults
    TEST_ASSERT_EQUAL_UINT16(0, f.getStartRegister());
    TEST_ASSERT_EQUAL_UINT16(0, f.getQuantity());
    TEST_ASSERT_EQUAL_UINT32(0, f.getByteCount());
    TEST_ASSERT_NULL(f.getRegisterData());

    // Act / Assert: set minimal data for start register
    f.dataLen = 2;
    f.data[0] = 0x01;
    f.data[1] = 0x02;
    TEST_ASSERT_EQUAL_UINT16(0x0102, f.getStartRegister());

    // Act / Assert: set data for quantity
    f.dataLen = 4;
    f.data[2] = 0x00;
    f.data[3] = 0x03;
    TEST_ASSERT_EQUAL_UINT16(0x0003, f.getQuantity());
}

/**
 * @brief Determine frame length for a request.
 * @goal Confirm that request frames are identified and their total length
 *       including CRC is returned.
 */
void test_determine_frame_length_request(void) {
    // Arrange:
    // - Build a canonical read request (unit=1, fc=3, start=0, qty=2)
    //   and append CRC so the helper sees a realistic frame.
    uint8_t msg[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    uint16_t crc = modbus_crc16(msg, 6);
    msg[6] = crc & 0xFF;
    msg[7] = (crc >> 8) & 0xFF;

    // Act
    bool isReq = false;
    size_t frameLen = 0;
    bool ok = ModbusRTUHelper::determineFrameLength(msg, sizeof(msg), isReq, frameLen);

    // Assert
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(isReq);
    TEST_ASSERT_EQUAL_UINT(8, frameLen);
}

/**
 * @brief Update register map from read holding request/response.
 * @goal Ensure register map reflects updated register values and metadata
 *       after processing a request/response pair.
 */
void test_update_register_map_holding(void) {
    // Arrange:
    // - Build a request targeting start=0 qty=2 and a matching response
    //   containing two 16-bit register values (0x000A, 0x0014).
    uint8_t req[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    uint16_t crc1 = modbus_crc16(req, 6);
    req[6] = crc1 & 0xFF; req[7] = (crc1 >> 8) & 0xFF;

    uint8_t resp[9] = { 0x01, 0x03, 0x04, 0x00, 0x0A, 0x00, 0x14, 0x00, 0x00 };
    uint16_t crc2 = modbus_crc16(resp, 7);
    resp[7] = crc2 & 0xFF; resp[8] = (crc2 >> 8) & 0xFF;

    ModbusFrame request;
    ModbusFrame response;

    // Act: parse frames and update map
    ModbusRTUHelper::parseModbusFrame(req, sizeof(req), request, 0, 0);
    ModbusRTUHelper::parseModbusFrame(resp, sizeof(resp), response, 0, 0);

    ModbusRegisterMap map{};
    map.unitId = 1;
    map.functionCode = 3;
    map.responseCount = 0;
    map.lastUpdate = 0;

    ModbusRTUHelper::updateModbusRegisterMap(map, request, response, 12345);

    // Assert: expect two registers updated and metadata set
    TEST_ASSERT_EQUAL_UINT16(10, map.registers[0]);
    TEST_ASSERT_EQUAL_UINT16(20, map.registers[1]);
    TEST_ASSERT_EQUAL_UINT32(1, map.responseCount);
    TEST_ASSERT_EQUAL_UINT32(12345, map.lastUpdate);
}

void setUp(void) {}
void tearDown(void) {}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_modbusframe_getters);
    RUN_TEST(test_determine_frame_length_request);
    RUN_TEST(test_update_register_map_holding);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_modbusframe_getters);
    RUN_TEST(test_determine_frame_length_request);
    RUN_TEST(test_update_register_map_holding);
    UNITY_END();
    return 0;
}
#endif
