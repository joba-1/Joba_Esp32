#/********************************************************************************
 * @file test_influx_line_protocol.cpp
 * @brief Tests for InfluxDB line protocol escaping helpers.
 *
 * Goal: Confirm escaping of measurement and tag strings so users can safely
 *       construct InfluxDB line-protocol payloads without accidental syntax errors.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include "InfluxLineProtocol_test.h"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Escape tag strings for InfluxDB line protocol.
 * @goal Verify that commas, equals and backslashes within tag strings are
 *       properly escaped so the resulting line protocol remains valid.
 */
static void test_escapeTag_basic(void) {
    // Arrange: use an input with commas, equals and backslashes to exercise escaping
    const char* in = "host,tag=val\\withspace";

    // Act
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeTag(in);

    // Assert
    TEST_ASSERT_EQUAL_STRING("host\\,tag\\=val\\\\withspace", out.c_str());
}

/**
 * @brief Escape measurement names for InfluxDB line protocol.
 * @goal Ensure spaces and commas in measurement names are escaped and that
 *       backslashes are preserved with proper escaping.
 */
static void test_escapeMeasurement_basic(void) {
    // Arrange: use a measurement containing space, comma and a backslash
    const char* in = "my measurement,with\\comma";

    // Act
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeMeasurement(in);

    // Assert
    TEST_ASSERT_EQUAL_STRING("my\\ measurement\\,with\\\\comma", out.c_str());
}

/**
 * @brief Null and empty input handling for tag escaping.
 * @goal Confirm that `escapeTag` gracefully handles `nullptr` by returning
 *       an empty string, avoiding crashes in callers.
 */
static void test_escapeTag_null_empty(void) {
    // Arrange: nullptr input
    // Act
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeTag(nullptr);
    
    // Assert
    TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_escapeTag_basic);
    RUN_TEST(test_escapeMeasurement_basic);
    RUN_TEST(test_escapeTag_null_empty);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_escapeTag_basic);
    RUN_TEST(test_escapeMeasurement_basic);
    RUN_TEST(test_escapeTag_null_empty);
    return UNITY_END();
}
#endif
