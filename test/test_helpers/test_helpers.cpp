#/********************************************************************************
 * @file test_helpers.cpp
 * @brief Trivial test helpers smoke test.
 *
 * Goal: Provide a minimal passing test to verify the native test harness
 *       is configured and can run tests on the host system.
 */

#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

/**
 * @brief Trivial pass smoke test.
 * @goal Verify the test harness runs at least one passing test on the host.
 */
void test_trivial_pass(void) {
    // Arrange:
    // - No setup required; this is a harness smoke test.
    // Rationale: keeping the test minimal ensures the harness itself is
    // functional without depending on other project components.
    // Act:
    // - Assert a true condition.
    // Assert:
    // - Expect the assertion to pass.
    TEST_ASSERT_TRUE(true);
}

#if defined(ARDUINO)
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_trivial_pass);
}

void loop() {
    UNITY_END();
    while (true) delay(1000);
}
#else
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_trivial_pass);
    return UNITY_END();
}
#endif
