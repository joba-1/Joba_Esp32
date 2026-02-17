#/********************************************************************************
 * @file test_time_utils.cpp
 * @brief Tests for TimeUtils helpers (validation and ISO formatting).
 *
 * Goal: Ensure time validation and ISO formatting behave as expected so users
 *       consuming timestamps in APIs receive correctly formatted values.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include "TimeUtils_test.h"

void setUp(void) {}
void tearDown(void) {}
/**
 * @brief Validate upper/lower bounds for Unix time validity.
 * @goal Ensure `isUnixTimeValid` accepts timestamps within the valid
 *       application window and rejects those outside it.
 */
static void test_isUnixTimeValid_bounds(void) {
    // Arrange: choose a value just inside and just outside the valid boundary
    // Act / Assert
    TEST_ASSERT_TRUE(TimeUtils::isUnixTimeValid((time_t)1600000001));
    TEST_ASSERT_FALSE(TimeUtils::isUnixTimeValid((time_t)1599999999));
}

/**
 * @brief Heuristic detection of Unix-second timestamps.
 * @goal Verify `looksLikeUnixSeconds` returns true for second-precision
 *       timestamps (large values) and false for small millisecond-like values.
 */
static void test_looksLikeUnixSeconds(void) {
    // Arrange: choose a typical recent unix-second and a clearly-non-second value
    // Act / Assert
    TEST_ASSERT_TRUE(TimeUtils::looksLikeUnixSeconds(1600000000UL));
    TEST_ASSERT_FALSE(TimeUtils::looksLikeUnixSeconds(1000UL));
}

/**
 * @brief ISO UTC formatting from Unix seconds.
 * @goal Confirm `isoUtcFromUnixSeconds` returns correctly formatted
 *       ISO timestamps for valid inputs and an empty string for invalid ones.
 */
static void test_isoUtcFromUnixSeconds_format(void) {
    // Arrange: valid and invalid timestamps
    // Act: format valid timestamp
    // 2023-01-01T00:00:00Z == 1672531200
    std::string s = TimeUtils::isoUtcFromUnixSeconds(1672531200U);

    // Assert: expect exact ISO string
    TEST_ASSERT_EQUAL_STRING("2023-01-01T00:00:00Z", s.c_str());
    
    // Act: format invalid timestamp
    std::string empty = TimeUtils::isoUtcFromUnixSeconds(1000U);
    
    // Assert: invalid timestamp returns empty
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
