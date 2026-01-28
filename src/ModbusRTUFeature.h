#ifndef MODBUS_RTU_FEATURE_H
#define MODBUS_RTU_FEATURE_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <vector>
#include <map>
#include <functional>
#include "Feature.h"
#include "LoggingFeature.h"

/**
 * @brief Modbus function codes
 */
namespace ModbusFC {
    constexpr uint8_t READ_COILS = 0x01;
    constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;
    constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;
    constexpr uint8_t READ_INPUT_REGISTERS = 0x04;
    constexpr uint8_t WRITE_SINGLE_COIL = 0x05;
    constexpr uint8_t WRITE_SINGLE_REGISTER = 0x06;
    constexpr uint8_t WRITE_MULTIPLE_COILS = 0x0F;
    constexpr uint8_t WRITE_MULTIPLE_REGISTERS = 0x10;
}

/**
 * @brief A single Modbus RTU frame (request or response)
 */
struct ModbusFrame {
    uint8_t unitId;
    uint8_t functionCode;
    std::vector<uint8_t> data;      // Payload without unit ID, FC, and CRC
    uint16_t crc;
    unsigned long timestamp;
    bool isValid;                   // CRC check passed
    bool isException;               // Exception response (FC | 0x80)
    uint8_t exceptionCode;
    
    // For read requests: extract start register and quantity
    uint16_t getStartRegister() const {
        if (data.size() >= 2) return (data[0] << 8) | data[1];
        return 0;
    }
    
    uint16_t getQuantity() const {
        if (data.size() >= 4) return (data[2] << 8) | data[3];
        return 0;
    }
    
    // For read responses: get register data
    size_t getByteCount() const {
        if (data.size() >= 1) return data[0];
        return 0;
    }
    
    const uint8_t* getRegisterData() const {
        if (data.size() > 1) return &data[1];
        return nullptr;
    }
};

/**
 * @brief Raw register data storage for a unit/function code combination
 */
struct ModbusRegisterMap {
    uint8_t unitId;
    uint8_t functionCode;
    std::map<uint16_t, uint16_t> registers;  // address -> value
    unsigned long lastUpdate;
    uint32_t requestCount;
    uint32_t responseCount;
    uint32_t errorCount;
};

/**
 * @brief Pending Modbus request
 */
struct ModbusPendingRequest {
    uint8_t unitId;
    uint8_t functionCode;
    uint16_t startRegister;
    uint16_t quantity;
    std::vector<uint16_t> writeData;  // For write requests
    std::function<void(bool success, const ModbusFrame& response)> callback;
    unsigned long queuedAt;
    uint8_t retries;
};

/**
 * @brief Low-level Modbus RTU bus monitor and master
 */
class ModbusRTUFeature : public Feature {
public:
    using FrameCallback = std::function<void(const ModbusFrame& frame, bool isRequest)>;
    
    /**
     * @brief Construct Modbus RTU feature
     * @param serial HardwareSerial instance (Serial1, Serial2)
     * @param baudRate Baud rate (9600, 19200, etc.)
     * @param config Serial config (SERIAL_8N1, SERIAL_8E1, etc.)
     * @param rxPin RX pin (-1 for default)
     * @param txPin TX pin (-1 for default)
     * @param dePin DE/RE pin for RS485 transceiver (-1 if not used)
     * @param maxQueueSize Maximum pending requests
     * @param responseTimeoutMs Response timeout in ms
     */
    ModbusRTUFeature(HardwareSerial& serial,
                     uint32_t baudRate = 9600,
                     uint32_t config = SERIAL_8N1,
                     int8_t rxPin = -1,
                     int8_t txPin = -1,
                     int8_t dePin = -1,
                     size_t maxQueueSize = 16,
                     uint32_t responseTimeoutMs = 1000);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "ModbusRTU"; }
    bool isReady() const override { return _ready; }
    
    // ========================================
    // Bus State
    // ========================================
    
    /**
     * @brief Check if bus is silent (no traffic for required time)
     */
    bool isBusSilent() const { return _busSilent; }
    
    /**
     * @brief Get time since last bus activity in ms
     */
    unsigned long getTimeSinceLastActivity() const {
        return millis() - _lastActivityTime;
    }
    
    /**
     * @brief Get the minimum silence time required (3.5 char times)
     */
    uint32_t getMinSilenceTimeUs() const { return _silenceTimeUs; }
    
    // ========================================
    // Bus Monitoring
    // ========================================
    
    /**
     * @brief Register callback for all frames seen on bus
     */
    void onFrame(FrameCallback callback) { _frameCallback = callback; }
    
    /**
     * @brief Get register map for a unit/function code combination
     */
    ModbusRegisterMap* getRegisterMap(uint8_t unitId, uint8_t functionCode);
    
    /**
     * @brief Get all register maps
     */
    const std::map<uint16_t, ModbusRegisterMap>& getAllRegisterMaps() const { return _registerMaps; }
    
    /**
     * @brief Read a register value from cache
     */
    bool readCachedRegister(uint8_t unitId, uint8_t functionCode, 
                            uint16_t address, uint16_t& value);
    
    // ========================================
    // Sending Requests
    // ========================================
    
    /**
     * @brief Queue a read registers request
     * @return true if queued successfully
     */
    bool queueReadRegisters(uint8_t unitId, uint8_t functionCode,
                            uint16_t startRegister, uint16_t quantity,
                            std::function<void(bool, const ModbusFrame&)> callback = nullptr);
    
    /**
     * @brief Queue a write single register request
     */
    bool queueWriteSingleRegister(uint8_t unitId, uint16_t address, uint16_t value,
                                   std::function<void(bool, const ModbusFrame&)> callback = nullptr);
    
    /**
     * @brief Queue a write multiple registers request
     */
    bool queueWriteMultipleRegisters(uint8_t unitId, uint16_t startAddress,
                                      const std::vector<uint16_t>& values,
                                      std::function<void(bool, const ModbusFrame&)> callback = nullptr);
    
    /**
     * @brief Send a raw frame (waits for bus silence)
     */
    bool sendRawFrame(const uint8_t* data, size_t length);
    
    /**
     * @brief Get pending request count
     */
    size_t getPendingRequestCount() const { return _requestQueue.size(); }
    
    /**
     * @brief Clear all pending requests
     */
    void clearQueue() { _requestQueue.clear(); }
    
    // ========================================
    // Statistics
    // ========================================
    
    struct Stats {
        uint32_t framesReceived;
        uint32_t framesSent;
        uint32_t crcErrors;
        uint32_t timeouts;
        uint32_t queueOverflows;
    };
    const Stats& getStats() const { return _stats; }

private:
    void processReceivedData();
    bool parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame);
    void updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response);
    void processQueue();
    bool sendRequest(const ModbusPendingRequest& request);
    void sendFrame(const std::vector<uint8_t>& frame);
    uint16_t calculateCRC(const uint8_t* data, size_t length);
    void setDE(bool transmit);
    
    static uint16_t makeMapKey(uint8_t unitId, uint8_t functionCode) {
        return (unitId << 8) | functionCode;
    }
    
    HardwareSerial& _serial;
    uint32_t _baudRate;
    uint32_t _config;
    int8_t _rxPin;
    int8_t _txPin;
    int8_t _dePin;
    size_t _maxQueueSize;
    uint32_t _responseTimeoutMs;
    
    uint32_t _silenceTimeUs;          // 3.5 character times in microseconds
    uint32_t _charTimeUs;             // Time for one character
    
    std::vector<uint8_t> _rxBuffer;
    unsigned long _lastByteTime;
    unsigned long _lastActivityTime;
    bool _busSilent;
    bool _ready;
    
    // Frame tracking for request/response matching
    ModbusFrame _lastRequest;
    bool _waitingForResponse;
    unsigned long _requestSentTime;
    
    // Register storage
    std::map<uint16_t, ModbusRegisterMap> _registerMaps;
    
    // Request queue
    std::vector<ModbusPendingRequest> _requestQueue;
    ModbusPendingRequest* _currentRequest;
    
    FrameCallback _frameCallback;
    Stats _stats;
};

#endif // MODBUS_RTU_FEATURE_H
