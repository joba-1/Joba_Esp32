#ifndef LOGGING_FEATURE_H
#define LOGGING_FEATURE_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include "Feature.h"

// Log levels
#define LOG_LEVEL_OFF     0
#define LOG_LEVEL_ERROR   1
#define LOG_LEVEL_WARN    2
#define LOG_LEVEL_INFO    3
#define LOG_LEVEL_DEBUG   4
#define LOG_LEVEL_VERBOSE 5

// Syslog facility codes
#define SYSLOG_FACILITY_USER   (1 << 3)   // User-level messages

// Syslog severity codes (RFC 5424)
#define SYSLOG_SEVERITY_EMERGENCY  0
#define SYSLOG_SEVERITY_ALERT      1
#define SYSLOG_SEVERITY_CRITICAL   2
#define SYSLOG_SEVERITY_ERROR      3
#define SYSLOG_SEVERITY_WARNING    4
#define SYSLOG_SEVERITY_NOTICE     5
#define SYSLOG_SEVERITY_INFO       6
#define SYSLOG_SEVERITY_DEBUG      7

/**
 * @brief Centralized logging feature with serial and syslog output
 * 
 * Supports:
 * - Separate log levels for serial and syslog
 * - Boot log level that transitions to runtime level after specified time
 * - Parallel output to syslog server via UDP
 */
class LoggingFeature : public Feature {
public:
    /**
     * @brief Construct logging feature
     * @param baudRate Serial baud rate
     * @param serialBootLogLevel Log level for serial during boot phase
     * @param serialRuntimeLogLevel Log level for serial after boot phase
     * @param bootDurationMs Duration of boot phase in milliseconds
     * @param syslogLogLevel Log level for syslog output
     * @param syslogServer Syslog server hostname/IP (empty = disabled)
     * @param syslogPort Syslog server port (typically 514)
     * @param hostname Device hostname for syslog messages
     * @param enableTimestamp Include timestamp in log messages
     */
    LoggingFeature(uint32_t baudRate, 
                   uint8_t serialBootLogLevel,
                   uint8_t serialRuntimeLogLevel,
                   uint32_t bootDurationMs,
                   uint8_t syslogLogLevel,
                   const char* syslogServer,
                   uint16_t syslogPort,
                   const char* hostname,
                   bool enableTimestamp);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "Logging"; }
    bool isReady() const override { return _ready; }
    
    void error(const char* format, ...);
    void warn(const char* format, ...);
    void info(const char* format, ...);
    void debug(const char* format, ...);
    void verbose(const char* format, ...);
    
    /**
     * @brief Get singleton instance for global logging
     */
    static LoggingFeature* getInstance();
    
    uint8_t getSerialLogLevel() const { return _serialLogLevel; }
    void setSerialLogLevel(uint8_t level) { _serialLogLevel = level; }
    
    uint8_t getSyslogLogLevel() const { return _syslogLogLevel; }
    void setSyslogLogLevel(uint8_t level) { _syslogLogLevel = level; }
    
    bool isSyslogEnabled() const { return _syslogEnabled; }
    bool isBootPhase() const { return _inBootPhase; }

private:
    void log(uint8_t level, const char* levelStr, const char* format, va_list args);
    void logToSerial(uint8_t level, const char* levelStr, const char* message);
    void logToSyslog(uint8_t level, const char* message);
    void printTimestamp();
    String getTimestamp();
    uint8_t logLevelToSyslogSeverity(uint8_t level);
    
    uint32_t _baudRate;
    uint8_t _serialBootLogLevel;
    uint8_t _serialRuntimeLogLevel;
    uint8_t _serialLogLevel;  // Current active serial log level
    uint32_t _bootDurationMs;
    uint8_t _syslogLogLevel;
    const char* _syslogServer;
    uint16_t _syslogPort;
    const char* _hostname;
    bool _enableTimestamp;
    bool _ready;
    bool _syslogEnabled;
    bool _inBootPhase;
    unsigned long _bootStartTime;
    
    WiFiUDP _udp;
    
    static LoggingFeature* _instance;
};

// Global convenience macros
#define LOG_E(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->error(__VA_ARGS__); } while(0)
#define LOG_W(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->warn(__VA_ARGS__); } while(0)
#define LOG_I(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->info(__VA_ARGS__); } while(0)
#define LOG_D(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->debug(__VA_ARGS__); } while(0)
#define LOG_V(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->verbose(__VA_ARGS__); } while(0)

#endif // LOGGING_FEATURE_H
