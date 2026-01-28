#ifndef LED_FEATURE_H
#define LED_FEATURE_H

#include "Feature.h"
#include <Arduino.h>

/**
 * @brief LED indicator feature
 * 
 * Controls an LED to indicate system activity:
 * - Stays on during setup phase
 * - Pulses briefly when data is received/transmitted
 */
class LEDFeature : public Feature {
public:
    /**
     * @brief Construct LED feature
     * @param pin GPIO pin number for LED
     * @param activeLow true if LED is active-low (common for built-in LEDs)
     * @param pulseDurationMs How long to keep LED on for pulses
     */
    LEDFeature(int8_t pin, bool activeLow = true, uint32_t pulseDurationMs = 50)
        : _pin(pin)
        , _activeLow(activeLow)
        , _pulseDurationMs(pulseDurationMs)
        , _setupComplete(false)
        , _pulseEndTime(0)
    {
    }
    
    void setup() override {
        if (_pin < 0) return;
        
        pinMode(_pin, OUTPUT);
        on();  // Turn on during setup
        _setupComplete = false;
    }
    
    void loop() override {
        if (!_setupComplete) {
            // Keep LED on during setup phase
            return;
        }
        
        // Handle pulse timeout
        if (_pulseEndTime > 0 && millis() >= _pulseEndTime) {
            off();
            _pulseEndTime = 0;
        }
    }
    
    const char* getName() const override {
        return "LED";
    }
    
    /**
     * @brief Call after setup is complete to turn off LED
     */
    void setupComplete() {
        _setupComplete = true;
        off();
    }
    
    /**
     * @brief Pulse LED briefly (for data activity)
     */
    void pulse() {
        if (_pin < 0) return;
        on();
        _pulseEndTime = millis() + _pulseDurationMs;
    }
    
    /**
     * @brief Turn LED on
     */
    void on() {
        if (_pin < 0) return;
        digitalWrite(_pin, _activeLow ? LOW : HIGH);
    }
    
    /**
     * @brief Turn LED off
     */
    void off() {
        if (_pin < 0) return;
        digitalWrite(_pin, _activeLow ? HIGH : LOW);
    }
    
    /**
     * @brief Toggle LED state
     */
    void toggle() {
        if (_pin < 0) return;
        digitalWrite(_pin, !digitalRead(_pin));
    }

private:
    int8_t _pin;
    bool _activeLow;
    uint32_t _pulseDurationMs;
    bool _setupComplete;
    unsigned long _pulseEndTime;
};

#endif // LED_FEATURE_H
