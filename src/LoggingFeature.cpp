#include "LoggingFeature.h"
#include <stdarg.h>
#include <time.h>

// Static instance pointer
LoggingFeature* LoggingFeature::_instance = nullptr;

LoggingFeature::LoggingFeature(uint32_t baudRate, uint8_t logLevel, bool enableTimestamp)
    : _baudRate(baudRate)
    , _logLevel(logLevel)
    , _enableTimestamp(enableTimestamp)
    , _ready(false)
{
    _instance = this;
}

void LoggingFeature::setup() {
    if (_ready) return;
    
    Serial.begin(_baudRate);
    
    // Non-blocking: don't wait for Serial to be ready
    // Just mark as ready and continue
    _ready = true;
    
    // Print startup message
    Serial.println();
    Serial.println("=================================");
    Serial.println("  ESP32 Firmware Starting...");
    Serial.println("=================================");
    Serial.println();
}

LoggingFeature* LoggingFeature::getInstance() {
    return _instance;
}

void LoggingFeature::printTimestamp() {
    if (!_enableTimestamp) return;
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        // We have valid time from NTP
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.print("[");
        Serial.print(buffer);
        Serial.print("] ");
    } else {
        // Use millis() as fallback
        Serial.print("[");
        Serial.print(millis());
        Serial.print("ms] ");
    }
}

void LoggingFeature::log(uint8_t level, const char* levelStr, const char* format, va_list args) {
    if (level > _logLevel || _logLevel == LOG_LEVEL_OFF) return;
    if (!_ready) return;
    
    printTimestamp();
    Serial.print(levelStr);
    Serial.print(": ");
    
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.println(buffer);
}

void LoggingFeature::error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LOG_LEVEL_ERROR, "ERROR", format, args);
    va_end(args);
}

void LoggingFeature::warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LOG_LEVEL_WARN, "WARN ", format, args);
    va_end(args);
}

void LoggingFeature::info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LOG_LEVEL_INFO, "INFO ", format, args);
    va_end(args);
}

void LoggingFeature::debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LOG_LEVEL_DEBUG, "DEBUG", format, args);
    va_end(args);
}

void LoggingFeature::verbose(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log(LOG_LEVEL_VERBOSE, "VERB ", format, args);
    va_end(args);
}
