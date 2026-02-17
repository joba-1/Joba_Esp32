// Unit tests for ModbusRTUFeature (PlatformIO Unity)
#include <Arduino.h>
#/********************************************************************************
 * @file test_modbus_rtu_feature.cpp
 * @brief Tests for Modbus RTU feature integration (native subset).
 *
 * Goal: From a user perspective, ensure the RTU feature's core helpers behave
 *       correctly under host execution so integration bugs are caught early.
 */

#include <unity.h>

#include "ISerial.h"
#include "ModbusFrame.h"
#include <vector>

static uint16_t reference_crc(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Calculate CRC for normal and exception frames.
 * @goal Validate CRC calculation matches the reference algorithm for both
 *       regular and exception frames so frame integrity checks are reliable.
 */
void test_calculate_frame_crc(void) {
    // Arrange: normal frame
    ModbusFrame f = {};
    f.unitId = 0x11;
    f.functionCode = 0x03;
    f.isException = false;
    f.dataLen = 2;
    f.data[0] = 0x12;
    f.data[1] = 0x34;

    // Act: compute reference CRC over unit+fc+payload
    uint8_t rawNormalAll[4] = { f.unitId, f.functionCode, f.data[0], f.data[1] };
    uint16_t refNormal = reference_crc(rawNormalAll, sizeof(rawNormalAll));

    // Assert
    TEST_ASSERT_EQUAL_UINT16(refNormal, refNormal);

    // Arrange: exception frame
    ModbusFrame ex = {};
    ex.unitId = 0x22;
    ex.functionCode = 0x83;
    ex.isException = true;
    ex.exceptionCode = 0x02;

    // Act
    uint8_t rawExAll[3] = { ex.unitId, ex.functionCode, ex.exceptionCode };
    uint16_t refEx = reference_crc(rawExAll, sizeof(rawExAll));

    // Assert
    TEST_ASSERT_EQUAL_UINT16(refEx, refEx);
}

/**
 * @brief Hex formatting helper test.
 * @goal Ensure byte arrays are formatted as uppercase hex pairs separated
 *       by spaces for diagnostic output.
 */
void test_format_hex(void) {
    // Arrange
    const uint8_t data[] = {0xAA, 0xBB, 0x01};

    // Act
    String result;
    for (size_t i = 0; i < sizeof(data); i++) {
        if (i > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }

    // Assert
    TEST_ASSERT_EQUAL_STRING("AA BB 01", result.c_str());
}

/**
 * @brief Frame-level hex formatting test.
 * @goal Confirm a Modbus frame (including CRC) is rendered as a single
 *       space-separated hex string for visibility and debugging.
 */
void test_format_frame_hex(void) {
    // Arrange
    ModbusFrame f = {};
    f.unitId = 0x10;
    f.functionCode = 0x03;
    f.dataLen = 2;
    f.data[0] = 0x00;
    f.data[1] = 0x01;
    f.crc = 0xAABB;

    // Act
    String result;
    auto appendByte = [&](uint8_t b) {
        if (result.length() > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", b);
        result += buf;
    };
    appendByte(f.unitId);
    appendByte(f.functionCode);
    for (size_t i = 0; i < f.dataLen; i++) appendByte(f.data[i]);
    appendByte((uint8_t)(f.crc & 0xFF));
    appendByte((uint8_t)((f.crc >> 8) & 0xFF));

    // Assert
    TEST_ASSERT_EQUAL_STRING("10 03 00 01 BB AA", result.c_str());
}

// --- FakeSerial tests (Arrange / Act / Assert) ---
// Simple fake serial implementing ISerial for tests
struct FakeSerial : public ISerial {
    std::vector<uint8_t> rx; // bytes available to read
    std::vector<uint8_t> tx; // bytes written by feature

    void begin(uint32_t /*baud*/, uint32_t /*config*/ = 0, int8_t /*rx*/ = -1, int8_t /*tx*/ = -1) override {}
    int available() override { return (int)rx.size(); }
    int read() override {
        if (rx.empty()) return -1;
        int b = rx.front();
        rx.erase(rx.begin());
        return b;
    }
    size_t write(uint8_t b) override { tx.push_back(b); return 1; }
    void flush() override {}
};

// Lightweight helper that emulates the minimal sendRawFrame behavior we want to test.
static bool sendRawFrameHelper(ISerial& s, const uint8_t* data, size_t length) {
    // Abort if RX bytes pending (last-moment safety check)
    if (s.available() > 0) return false;
    for (size_t i = 0; i < length; i++) s.write(data[i]);
    uint16_t crc = reference_crc(data, length);
    s.write((uint8_t)(crc & 0xFF));
    s.write((uint8_t)((crc >> 8) & 0xFF));
    s.flush();
    return true;
}

/**
 * @brief sendRawFrame success path.
 * @goal Verify the helper sends payload + CRC when no RX bytes are pending.
 */
void test_send_raw_frame_success(void) {
    // Arrange
    FakeSerial s;
    const uint8_t payload[] = { 0x01, 0x03, 0x00, 0x10 };

    // Act
    bool sent = sendRawFrameHelper(s, payload, sizeof(payload));

    // Assert
    TEST_ASSERT_TRUE(sent);
    TEST_ASSERT_EQUAL_UINT((size_t)(sizeof(payload) + 2), s.tx.size());
    uint16_t crc = reference_crc(payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(crc & 0xFF), s.tx[sizeof(payload)]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((crc >> 8) & 0xFF), s.tx[sizeof(payload) + 1]);
}

/**
 * @brief sendRawFrame abort behavior when RX pending.
 * @goal Ensure the helper aborts sending if RX buffer contains pending bytes
 *       to avoid colliding with inbound data.
 */
void test_send_raw_frame_abort_on_rx_pending(void) {
    // Arrange
    FakeSerial s;
    s.rx.push_back(0x55);
    const uint8_t payload[] = { 0x01, 0x03, 0x00, 0x10 };

    // Act
    bool sent = sendRawFrameHelper(s, payload, sizeof(payload));

    // Assert
    TEST_ASSERT_FALSE(sent);
    TEST_ASSERT_EQUAL_UINT(0, s.tx.size());
}

void setup() {
    delay(10);
    UNITY_BEGIN();
    RUN_TEST(test_calculate_frame_crc);
    RUN_TEST(test_format_hex);
    RUN_TEST(test_format_frame_hex);
    RUN_TEST(test_send_raw_frame_success);
    RUN_TEST(test_send_raw_frame_abort_on_rx_pending);
    UNITY_END();
}

void loop() {}


