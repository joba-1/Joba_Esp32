#include "LoggingFeature.h"
#include <stdarg.h>
#include <time.h>
#include <WiFi.h>

// Static instance pointer
LoggingFeature* LoggingFeature::_instance = nullptr;

LoggingFeature::LoggingFeature(uint32_t baudRate,
                               uint8_t serialBootLogLevel,
                               uint8_t serialRuntimeLogLevel,
                               uint32_t bootDurationMs,
                               uint8_t syslogLogLevel,
                               const char* syslogServer,
                               uint16_t syslogPort,
                               const char* hostname,
                               bool enableTimestamp)
    : _baudRate(baudRate)
    , _serialBootLogLevel(serialBootLogLevel)
    , _serialRuntimeLogLevel(serialRuntimeLogLevel)
    , _serialLogLevel(serialBootLogLevel)  // Start with boot level
    , _bootDurationMs(bootDurationMs)
    , _syslogLogLevel(syslogLogLevel)
    , _syslogServer(syslogServer)
    , _syslogPort(syslogPort)
    , _hostname(hostname)
    , _enableTimestamp(enableTimestamp)
    , _ready(false)
    , _syslogEnabled(strlen(syslogServer) > 0)
    , _inBootPhase(true)
    , _bootStartTime(0)
{
    _instance = this;
}

void LoggingFeature::setup() {
    if (_ready) return;
    
    Serial.begin(_baudRate);
    _bootStartTime = millis();
    
    // Non-blocking: don't wait for Serial to be ready
    // Just mark as ready and continue
    _ready = true;
    
    // Print startup message
    Serial.println();
    Serial.println("=================================");
    Serial.println("  ESP32 Firmware Starting...");
    Serial.println("=================================");
    Serial.printf("  Serial boot log level: %d\n", _serialBootLogLevel);
    Serial.printf("  Serial runtime log level: %d\n", _serialRuntimeLogLevel);
    Serial.printf("  Boot phase duration: %lu ms\n", _bootDurationMs);
    if (_syslogEnabled) {
        Serial.printf("  Syslog: %s:%d (level %d)\n", _syslogServer, _syslogPort, _syslogLogLevel);
    } else {
        Serial.println("  Syslog: disabled");
    }
    Serial.println("=================================");
    Serial.println();
}

void LoggingFeature::loop() {
    // Check if boot phase has ended
    if (_inBootPhase && (millis() - _bootStartTime >= _bootDurationMs)) {
        _inBootPhase = false;
        _serialLogLevel = _serialRuntimeLogLevel;
        info("Boot phase ended, serial log level changed to %d", _serialRuntimeLogLevel);
    }
}

LoggingFeature* LoggingFeature::getInstance() {
    return _instance;
}

String LoggingFeature::getTimestamp() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        // We have valid time from NTP
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        return String(buffer);
    } else {
        // Use millis() as fallback
        return String(millis()) + "ms";
    }
}

void LoggingFeature::printTimestamp() {
    if (!_enableTimestamp) return;
    Serial.print("[");
    Serial.print(getTimestamp());
    Serial.print("] ");
}

uint8_t LoggingFeature::logLevelToSyslogSeverity(uint8_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR:   return SYSLOG_SEVERITY_ERROR;
        case LOG_LEVEL_WARN:    return SYSLOG_SEVERITY_WARNING;
        case LOG_LEVEL_INFO:    return SYSLOG_SEVERITY_INFO;
        case LOG_LEVEL_DEBUG:   return SYSLOG_SEVERITY_DEBUG;
        case LOG_LEVEL_VERBOSE: return SYSLOG_SEVERITY_DEBUG;
        default:                return SYSLOG_SEVERITY_INFO;
    }
}

void LoggingFeature::logToSerial(uint8_t level, const char* levelStr, const char* message) {
    if (level > _serialLogLevel || _serialLogLevel == LOG_LEVEL_OFF) return;
    
    printTimestamp();
    Serial.print(levelStr);
    Serial.print(": ");
    Serial.println(message);
}

void LoggingFeature::logToSyslog(uint8_t level, const char* message) {
    if (!_syslogEnabled) return;
    if (level > _syslogLogLevel || _syslogLogLevel == LOG_LEVEL_OFF) return;
    if (WiFi.status() != WL_CONNECTED) return;
    
    // Calculate PRI value: facility * 8 + severity
    uint8_t severity = logLevelToSyslogSeverity(level);
    uint8_t pri = SYSLOG_FACILITY_USER + severity;
    
    // Build syslog message (RFC 3164 BSD format)
    // <PRI>TIMESTAMP HOSTNAME TAG: MESSAGE
    char syslogMsg[512];
    
    struct tm timeinfo;
    char timestamp[20];
    if (getLocalTime(&timeinfo, 0)) {
        // RFC 3164 timestamp format: "Mmm dd hh:mm:ss"
        strftime(timestamp, sizeof(timestamp), "%b %d %H:%M:%S", &timeinfo);
    } else {
        strcpy(timestamp, "-");
    }
    
    snprintf(syslogMsg, sizeof(syslogMsg), "<%d>%s %s %s: %s",
             pri, timestamp, _hostname, FIRMWARE_NAME, message);
    
    // Send via UDP
    _udp.beginPacket(_syslogServer, _syslogPort);
    _udp.write((const uint8_t*)syslogMsg, strlen(syslogMsg));
    _udp.endPacket();
}

void LoggingFeature::log(uint8_t level, const char* levelStr, const char* format, va_list args) {
    if (!_ready) return;
    
    // Format the message once
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    
    // Output to serial (respects serial log level)
    logToSerial(level, levelStr, buffer);
    
    // Output to syslog (respects syslog log level)
    logToSyslog(level, buffer);
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
