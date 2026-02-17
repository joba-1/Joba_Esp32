#/********************************************************************************
 * @file test_influx_line_protocol.cpp
 * @brief Tests for InfluxDB line protocol escaping helpers.
 *
 * Goal: Confirm escaping of measurement and tag strings so users can safely
 *       construct InfluxDB line-protocol payloads without accidental syntax errors.
 */

#include <unity.h>
#include "InfluxLineProtocol_test.h"

void setUp(void) {}
void tearDown(void) {}

static void test_escapeTag_basic(void) {
    const char* in = "host,tag=val\\withspace";
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeTag(in);
    TEST_ASSERT_EQUAL_STRING("host\\,tag\\=val\\\\withspace", out.c_str());
}

static void test_escapeMeasurement_basic(void) {
    const char* in = "my measurement,with\\comma";
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeMeasurement(in);
    TEST_ASSERT_EQUAL_STRING("my\\ measurement\\,with\\\\comma", out.c_str());
}

static void test_escapeTag_null_empty(void) {
    InfluxLineProtocol::String out = InfluxLineProtocol::escapeTag(nullptr);
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
