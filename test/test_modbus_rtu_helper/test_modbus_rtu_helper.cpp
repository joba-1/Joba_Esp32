#include <unity.h>
#include "ModbusRTUFeature_helper.h"
#include "modbus_helpers.h"
#include "ModbusFrame.h"
#include "ModbusRegisterMap.h"
#include "ModbusFC.h"
#include <vector>
#include <cstring>

using namespace ModbusRTUHelper;

// Include the implementation so the symbol `modbus_crc16` is available
#include "../../src/modbus_helpers.cpp"

static std::vector<uint8_t> make_frame(const std::vector<uint8_t>& payload_without_crc) {
    std::vector<uint8_t> frame = payload_without_crc;
    uint16_t crc = modbus_crc16(frame.data(), frame.size());
    frame.push_back((uint8_t)(crc & 0xFF));
    frame.push_back((uint8_t)((crc >> 8) & 0xFF));
    return frame;
}

void test_determine_frame_length_short() {
    // Arrange
    bool isReq=false; size_t len=0;
    uint8_t buf[3] = {1,2,3};
    // Act & Assert
    TEST_ASSERT_FALSE(determineFrameLength(buf, 3, isReq, len));
}

void test_determine_frame_length_exception_response() {
    // Exception response for FC3: [unit, FC3|0x80, exCode, CRC]
    std::vector<uint8_t> raw = {0x11, (uint8_t)(ModbusFC::READ_HOLDING_REGISTERS | 0x80), 0x02};
    auto frame = make_frame(raw);
    // Arrange
    bool isReq=false; size_t len=0;
    // Act
    TEST_ASSERT_TRUE(determineFrameLength(frame.data(), frame.size(), isReq, len));
    // Assert
    TEST_ASSERT_FALSE(isReq);
    TEST_ASSERT_EQUAL(5u, len);
}

void test_determine_frame_length_request_and_response() {
    // Request: unit, FC3, start(2), qty(2), CRC => length 8
    std::vector<uint8_t> req = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x00, 0x10, 0x00, 0x02};
    auto reqf = make_frame(req);
    // Arrange
    bool isReq=false; size_t len=0;
    // Act
    TEST_ASSERT_TRUE(determineFrameLength(reqf.data(), reqf.size(), isReq, len));
    // Assert
    TEST_ASSERT_TRUE(isReq);
    TEST_ASSERT_EQUAL(8u, len);

    // Response: unit, FC3, byteCount(4), reg1 hi,lo, reg2 hi,lo, CRC => length 9
    std::vector<uint8_t> resp = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x04, 0x00,0x0A, 0x00,0x14};
    auto respf = make_frame(resp);
    // Arrange
    isReq=false; len=0;
    // Act
    TEST_ASSERT_TRUE(determineFrameLength(respf.data(), respf.size(), isReq, len));
    // Assert
    TEST_ASSERT_FALSE(isReq);
    TEST_ASSERT_EQUAL(9u, len);
}

void test_parse_modbus_frame_crc_invalid_and_exception_and_valid() {
    // Valid response frame
    std::vector<uint8_t> resp = {0x02, ModbusFC::READ_INPUT_REGISTERS, 0x02, 0x01, 0x02};
    auto respf = make_frame(resp);
    ModbusFrame frame;
    // Act
    TEST_ASSERT_TRUE(parseModbusFrame(respf.data(), respf.size(), frame, 0, 0));
    // Assert
    TEST_ASSERT_TRUE(frame.isValid);
    TEST_ASSERT_FALSE(frame.isException);
    TEST_ASSERT_EQUAL(2u, frame.getByteCount());

    // Corrupt CRC
    // Corrupt CRC
    respf.back() ^= 0xFF; // flip last CRC byte
    ModbusFrame frame2;
    // Act
    TEST_ASSERT_TRUE(parseModbusFrame(respf.data(), respf.size(), frame2, 0, 0));
    // Assert
    TEST_ASSERT_FALSE(frame2.isValid);

    // Exception frame
    std::vector<uint8_t> exc = {0x03, (uint8_t)(ModbusFC::READ_HOLDING_REGISTERS | 0x80), 0x05};
    auto excf = make_frame(exc);
    ModbusFrame frame3;
    // Act
    TEST_ASSERT_TRUE(parseModbusFrame(excf.data(), excf.size(), frame3, 0, 0));
    // Assert
    TEST_ASSERT_TRUE(frame3.isValid);
    TEST_ASSERT_TRUE(frame3.isException);
    TEST_ASSERT_EQUAL(0x05, frame3.exceptionCode);
}

void test_update_modbus_register_map_holding_and_coils() {
    ModbusRegisterMap regMap{};
    regMap.unitId = 1;
    regMap.functionCode = ModbusFC::READ_HOLDING_REGISTERS;
    regMap.requestCount = regMap.responseCount = regMap.errorCount = 0;

    // Build request: start=0x0010, qty=2
    std::vector<uint8_t> req = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x00,0x10, 0x00,0x02};
    auto reqf = make_frame(req);
    ModbusFrame reqFrame;
    parseModbusFrame(reqf.data(), reqf.size(), reqFrame, 0, 0);

    // Response with two registers: values 0x0011 and 0x0022
    std::vector<uint8_t> resp = {0x01, ModbusFC::READ_HOLDING_REGISTERS, 0x04, 0x00,0x11, 0x00,0x22};
    auto respf = make_frame(resp);
    ModbusFrame respFrame;
    parseModbusFrame(respf.data(), respf.size(), respFrame, 0, 0);

    // Act
    updateModbusRegisterMap(regMap, reqFrame, respFrame, 12345);
    // Assert
    TEST_ASSERT_EQUAL(1u, regMap.responseCount);
    TEST_ASSERT_EQUAL(12345u, regMap.lastUpdate);
    TEST_ASSERT_EQUAL(0x0011u, regMap.registers[0x0010]);
    TEST_ASSERT_EQUAL(0x0022u, regMap.registers[0x0011]);

    // Coils: use READ_COILS and bit-packed data (e.g., 1 coil true, others false)
    ModbusRegisterMap coilMap{};
    coilMap.unitId = 1;
    coilMap.functionCode = ModbusFC::READ_COILS;

    std::vector<uint8_t> coilReq = {0x01, ModbusFC::READ_COILS, 0x00,0x20, 0x00,0x08};
    auto coilReqF = make_frame(coilReq);
    ModbusFrame coilReqFrame; parseModbusFrame(coilReqF.data(), coilReqF.size(), coilReqFrame,0,0);

    // Response byteCount=1, bits LSB first: 0b00000001 -> coil 0 true
    std::vector<uint8_t> coilResp = {0x01, ModbusFC::READ_COILS, 0x01, 0x01};
    auto coilRespF = make_frame(coilResp);
    ModbusFrame coilRespFrame; parseModbusFrame(coilRespF.data(), coilRespF.size(), coilRespFrame,0,0);

    // Act
    updateModbusRegisterMap(coilMap, coilReqFrame, coilRespFrame, 54321);
    // Assert
    TEST_ASSERT_EQUAL(1u, coilMap.responseCount);
    TEST_ASSERT_EQUAL(54321u, coilMap.lastUpdate);
    TEST_ASSERT_EQUAL(1u, coilMap.registers[0x0020]); // first coil at startReg
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_determine_frame_length_short);
    RUN_TEST(test_determine_frame_length_exception_response);
    RUN_TEST(test_determine_frame_length_request_and_response);
    RUN_TEST(test_parse_modbus_frame_crc_invalid_and_exception_and_valid);
    RUN_TEST(test_update_modbus_register_map_holding_and_coils);
    return UNITY_END();
}
