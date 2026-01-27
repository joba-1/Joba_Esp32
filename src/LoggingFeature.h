#ifndef LOGGING_FEATURE_H
#define LOGGING_FEATURE_H

#include <Arduino.h>
#include "Feature.h"

// Log levels
#define LOG_LEVEL_OFF     0
#define LOG_LEVEL_ERROR   1
#define LOG_LEVEL_WARN    2
#define LOG_LEVEL_INFO    3
#define LOG_LEVEL_DEBUG   4
#define LOG_LEVEL_VERBOSE 5

/**
 * @brief Centralized logging feature with configurable output
 */
class LoggingFeature : public Feature {
public:
    /**
     * @brief Construct logging feature
     * @param baudRate Serial baud rate
     * @param logLevel Log level (0=OFF, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE)
     * @param enableTimestamp Include timestamp in log messages
     */
    LoggingFeature(uint32_t baudRate, uint8_t logLevel, bool enableTimestamp);
    
    void setup() override;
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
    
    uint8_t getLogLevel() const { return _logLevel; }
    void setLogLevel(uint8_t level) { _logLevel = level; }

private:
    void log(uint8_t level, const char* levelStr, const char* format, va_list args);
    void printTimestamp();
    
    uint32_t _baudRate;
    uint8_t _logLevel;
    bool _enableTimestamp;
    bool _ready;
    
    static LoggingFeature* _instance;
};

// Global convenience macros
#define LOG_E(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->error(__VA_ARGS__); } while(0)
#define LOG_W(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->warn(__VA_ARGS__); } while(0)
#define LOG_I(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->info(__VA_ARGS__); } while(0)
#define LOG_D(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->debug(__VA_ARGS__); } while(0)
#define LOG_V(...) do { if (LoggingFeature::getInstance()) LoggingFeature::getInstance()->verbose(__VA_ARGS__); } while(0)

#endif // LOGGING_FEATURE_H
