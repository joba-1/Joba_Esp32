#include <unity.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

void test_trivial_pass(void) {
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
