#/********************************************************************************
 * @file test_time_utils.cpp
 * @brief Tests for TimeUtils helpers (validation and ISO formatting).
 *
 * Goal: Ensure time validation and ISO formatting behave as expected so users
 *       consuming timestamps in APIs receive correctly formatted values.
 */

#include <unity.h>
#include "TimeUtils_test.h"

void setUp(void) {}
void tearDown(void) {}

static void test_isUnixTimeValid_bounds(void) {
    TEST_ASSERT_TRUE(TimeUtils::isUnixTimeValid((time_t)1600000001));
    TEST_ASSERT_FALSE(TimeUtils::isUnixTimeValid((time_t)1599999999));
}

static void test_looksLikeUnixSeconds(void) {
    TEST_ASSERT_TRUE(TimeUtils::looksLikeUnixSeconds(1600000000UL));
    TEST_ASSERT_FALSE(TimeUtils::looksLikeUnixSeconds(1000UL));
}

static void test_isoUtcFromUnixSeconds_format(void) {
    // 2023-01-01T00:00:00Z == 1672531200
    std::string s = TimeUtils::isoUtcFromUnixSeconds(1672531200U);
    TEST_ASSERT_EQUAL_STRING("2023-01-01T00:00:00Z", s.c_str());
    // invalid timestamp returns empty
    std::string empty = TimeUtils::isoUtcFromUnixSeconds(1000U);
    TEST_ASSERT_EQUAL_STRING("", empty.c_str());
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_isUnixTimeValid_bounds);
    RUN_TEST(test_looksLikeUnixSeconds);
    RUN_TEST(test_isoUtcFromUnixSeconds_format);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_isUnixTimeValid_bounds);
    RUN_TEST(test_looksLikeUnixSeconds);
    RUN_TEST(test_isoUtcFromUnixSeconds_format);
    return UNITY_END();
}
#endif
