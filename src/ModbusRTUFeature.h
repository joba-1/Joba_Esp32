#ifndef MODBUS_RTU_FEATURE_H
#define MODBUS_RTU_FEATURE_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <array>
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
    static constexpr size_t MAX_DATA_LEN = 252;  // up to FC3/FC4 response payload (byteCount+data)

    uint8_t unitId;
    uint8_t functionCode;
    std::array<uint8_t, MAX_DATA_LEN> data{};  // Payload without unit ID, FC, and CRC
    uint16_t dataLen{0};
    uint16_t crc;
    unsigned long timestamp;         // millis() at capture time (monotonic)
    uint32_t unixTimestamp;          // epoch seconds at capture time (0 if time invalid)
    bool isRequest;                  // request vs response (best-effort)
    bool isValid;                   // CRC check passed
    bool isException;               // Exception response (FC | 0x80)
    uint8_t exceptionCode;
    
    // For read requests: extract start register and quantity
    uint16_t getStartRegister() const {
        if (dataLen >= 2) return (data[0] << 8) | data[1];
        return 0;
    }
    
    uint16_t getQuantity() const {
        if (dataLen >= 4) return (data[2] << 8) | data[3];
        return 0;
    }
    
    // For read responses: get register data
    size_t getByteCount() const {
        if (dataLen >= 1) return data[0];
        return 0;
    }
    
    const uint8_t* getRegisterData() const {
        if (dataLen > 1) return &data[1];
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

    // ========================================
    // Debug (for /api/modbus/status)
    // ========================================
    uint32_t getTimeSinceLastByteUs() const { return (uint32_t)(micros() - _lastByteTime); }
    uint32_t getCharTimeUs() const { return _charTimeUs; }
    uint32_t getSilenceTimeUs() const { return _silenceTimeUs; }
    uint32_t getLoopCounter() const { return _loopCounter; }
    uint32_t getProcessQueueCounter() const { return _processQueueCounter; }
    unsigned long getLastProcessQueueMs() const { return _lastProcessQueueMs; }
    uint16_t getDbgQueueSizeInLoop() const { return _dbgQueueSizeInLoop; }
    bool getDbgWaitingForResponseInLoop() const { return _dbgWaitingForResponseInLoop; }
    uint16_t getDbgSerialAvailableInLoop() const { return _dbgSerialAvailableInLoop; }
    uint16_t getDbgRxBytesDrainedInLoop() const { return _dbgRxBytesDrainedInLoop; }
    uint32_t getDbgGapUsInLoop() const { return _dbgGapUsInLoop; }
    bool getDbgGapEnoughForTxInLoop() const { return _dbgGapEnoughForTxInLoop; }
    unsigned long getDbgLastLoopSnapshotMs() const { return _dbgLastLoopSnapshotMs; }
    
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
     * @brief Get queued request count (not including in-flight)
     */
    size_t getQueuedRequestCount() const { return _requestQueue.size(); }

    /**
     * @brief Get pending request count (queued + in-flight)
     */
    size_t getPendingRequestCount() const {
        return _requestQueue.size() + ((_waitingForResponse && _hasPendingRequest) ? 1 : 0);
    }

    /**
     * @brief True if a request is currently in-flight (TX sent, awaiting response)
     */
    bool isWaitingForResponse() const { return _waitingForResponse; }

    /**
     * @brief Timeout backoff state (sending may be paused after repeated timeouts)
     */
    bool isQueueingPaused() const;
    uint32_t getQueueingPauseRemainingMs() const;
    uint32_t getQueueingPausedUntilMs() const;
    uint32_t getQueueingBackoffMs() const;
    uint32_t getConsecutiveTimeouts() const;

    bool isUnitQueueingPaused(uint8_t unitId) const;
    uint32_t getUnitQueueingPauseRemainingMs(uint8_t unitId) const;
    uint32_t getUnitQueueingBackoffMs(uint8_t unitId) const;
    uint32_t getUnitConsecutiveTimeouts(uint8_t unitId) const;

    struct UnitBackoffInfo {
        uint8_t unitId;
        uint32_t consecutiveTimeouts;
        uint32_t backoffMs;
        uint32_t pausedUntilMs;
        bool paused;
        uint32_t pauseRemainingMs;
    };
    std::vector<UnitBackoffInfo> getUnitBackoffInfo() const;
    
    /**
     * @brief Clear all pending requests
     */
    void clearQueue() { _requestQueue.clear(); }
    
    /**
     * @brief Suspend all Modbus communication (for OTA, etc.)
     * Clears queue, stops polling, ignores incoming data
     */
    void suspend();
    
    /**
     * @brief Resume Modbus communication after suspend
     */
    void resume();
    
    /**
     * @brief Check if Modbus is currently suspended
     */
    bool isSuspended() const { return _suspended; }
    
    // ========================================
    // Statistics
    // ========================================
    
    struct Stats {
        // Request/Response counts - Our requests (cumulative since boot/reset)
        uint32_t ownRequestsSent;
        uint32_t ownRequestsSuccess;
        uint32_t ownRequestsFailed;      // Timeout or exception
        uint32_t ownRequestsDiscarded;   // Queue full
        
        // Request/Response counts - Other devices (monitored, cumulative)
        uint32_t otherRequestsSeen;
        uint32_t otherResponsesSeen;
        uint32_t otherExceptionsSeen;

        // Pairing quality for other devices (FC3/FC4 only, best-effort)
        uint32_t otherResponsesPaired;       // response matched to an observed request
        uint32_t otherResponsesUnpaired;     // response without a matching observed request
        uint32_t otherExceptionsPaired;      // exception matched to an observed request
        uint32_t otherExceptionsUnpaired;    // exception without a matching observed request
        
        // Legacy/general counts (cumulative)
        uint32_t framesReceived;
        uint32_t framesSent;
        uint32_t crcErrors;
        uint32_t timeouts;
        uint32_t queueOverflows;
        
        // Timing statistics (microseconds, cumulative)
        uint64_t ownActiveTimeUs;        // Time spent on our communication
        uint64_t otherActiveTimeUs;      // Time with other traffic
        uint64_t totalTimeUs;            // Total tracked time
        
        // For calculating active time
        unsigned long lastStatsReset;
    };
    
    // Interval-based stats for percentage calculations
    struct IntervalStats {
        uint32_t ownSuccess;
        uint32_t ownFailed;
        uint32_t otherSuccess;          // otherResponsesSeen
        uint32_t otherFailed;           // otherExceptionsSeen
        uint64_t ownActiveTimeUs;
        uint64_t otherActiveTimeUs;
        unsigned long intervalStartMs;  // Start time of current interval
    };
    
    const Stats& getStats() const { return _stats; }
    const IntervalStats& getIntervalStats() const { return _intervalStats; }
    
    /**
     * @brief Get failure rate for own requests in current interval (0.0 - 1.0)
     */
    float getOwnFailureRate() const;
    
    /**
     * @brief Get failure rate for other devices in current interval (0.0 - 1.0)
     */
    float getOtherFailureRate() const;
    
    /**
     * @brief Get bus idle percentage in current interval (0.0 - 100.0)
     */
    float getBusIdlePercent() const;
    
    /**
     * @brief Reset statistics (cumulative)
     */
    void resetStats();
    
    /**
     * @brief Reset interval statistics (called after each warning check)
     */
    void resetIntervalStats();
    
    String formatHex(const uint8_t* data, size_t length) const;

    /**
     * @brief Format a full Modbus RTU frame as hex (unit + fc + payload + CRC bytes)
     */
    String formatFrameHex(const ModbusFrame& frame) const;

    /**
     * @brief Calculate CRC16 for a parsed frame (unit + fc + payload/exception)
     *
     * Note: ModbusFrame::crc stores the CRC received on the wire.
     */
    uint16_t calculateFrameCrc(const ModbusFrame& frame) const;

    struct CrcErrorContext {
        uint32_t id{0};
        bool hasBefore{false};
        bool hasAfter{false};
        ModbusFrame before;
        ModbusFrame bad;
        ModbusFrame after;
    };

    /**
     * @brief Get recent CRC error contexts (ring buffer)
     */
    const CrcErrorContext* getRecentCrcErrorContexts(size_t& outCount) const;

private:
    void processReceivedData();
    bool parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame);
    void recordFrameToHistory(const ModbusFrame& frame);
    void recordCrcErrorContext(const ModbusFrame& badFrame);
    ModbusRegisterMap& ensureRegisterMap(uint8_t unitId, uint8_t functionCode);
    void updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response);
    void processQueue();
    bool sendRequest(const ModbusPendingRequest& request);
    void sendFrame(const std::vector<uint8_t>& frame);
    uint16_t calculateCRC(const uint8_t* data, size_t length) const;
    void setDE(bool transmit);
    void checkAndLogWarnings();
    void startActiveTime(bool isOwn);
    void endActiveTime();
    
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
    
    bool _suspended{false};           // When true, skip all processing (for OTA)
    
    std::vector<uint8_t> _rxBuffer;
    unsigned long _lastByteTime;
    unsigned long _lastActivityTime;
    bool _busSilent;
    bool _ready;

    // Serial buffer emptiness tracking (best-effort for TX arbitration when loop is slow)
    bool _serialWasEmpty{true};
    unsigned long _serialEmptySinceUs{0};
    
    // Frame tracking for request/response matching
    ModbusFrame _lastRequest;
    std::map<uint8_t, ModbusFrame> _lastRequestPerUnit;
    bool _waitingForResponse;
    unsigned long _requestSentTime;
    
    // Register storage
    std::map<uint16_t, ModbusRegisterMap> _registerMaps;
    
    // Request queue
    std::vector<ModbusPendingRequest> _requestQueue;
    ModbusPendingRequest _currentRequest;  // Copy, not pointer - prevents invalid references
    bool _hasPendingRequest;

    struct TimeoutBackoffState {
        uint32_t consecutiveTimeouts{0};
        uint32_t backoffMs{2000};
        uint32_t pausedUntilMs{0};
    };
    std::map<uint8_t, TimeoutBackoffState> _backoffByUnit;

    unsigned long _lastSuccessTime;  // Time of last successful request
    unsigned long _lastTimeoutWarningMs;  // Throttle timeout warning messages
    std::map<uint16_t, unsigned long> _lastTimeoutPerUnit;  // Track last timeout per unit (throttle spam)
    
    FrameCallback _frameCallback;
    Stats _stats;
    IntervalStats _intervalStats;
    
    // Timing tracking
    bool _inActiveTime;
    bool _activeTimeIsOwn;
    unsigned long _activeStartTimeUs;
    unsigned long _lastWarningCheckMs;

    // Debug counters/timestamps
    uint32_t _loopCounter{0};
    uint32_t _processQueueCounter{0};
    unsigned long _lastProcessQueueMs{0};

    // Last-loop debug snapshot (best-effort; used for diagnostics only)
    uint16_t _dbgQueueSizeInLoop{0};
    bool _dbgWaitingForResponseInLoop{false};
    uint16_t _dbgSerialAvailableInLoop{0};
    uint16_t _dbgRxBytesDrainedInLoop{0};
    uint32_t _dbgGapUsInLoop{0};
    bool _dbgGapEnoughForTxInLoop{false};
    unsigned long _dbgLastLoopSnapshotMs{0};
    
    // Configurable warning thresholds (set via build flags with defaults)
#ifndef MODBUS_STATS_INTERVAL_MS
#define MODBUS_STATS_INTERVAL_MS 60000
#endif
#ifndef MODBUS_OWN_FAIL_WARN_PERCENT
#define MODBUS_OWN_FAIL_WARN_PERCENT 5
#endif
#ifndef MODBUS_OTHER_FAIL_WARN_PERCENT
#define MODBUS_OTHER_FAIL_WARN_PERCENT 5
#endif
#ifndef MODBUS_BUS_BUSY_WARN_PERCENT
#define MODBUS_BUS_BUSY_WARN_PERCENT 95
#endif

#ifndef MODBUS_LISTEN_ONLY
#define MODBUS_LISTEN_ONLY 0
#endif
    
    static const size_t FRAME_HISTORY_SIZE = 20;
    ModbusFrame _frameHistory[FRAME_HISTORY_SIZE];
    size_t _frameHistoryIndex = 0;
    mutable std::vector<ModbusFrame> _recentFramesCache;

    // RX buffer timing (best-effort start-of-buffer timestamps)
    uint32_t _rxBufferStartUs{0};
    uint32_t _rxBufferStartMs{0};

    // CRC error contexts (before/bad/after)
    static const size_t CRC_CONTEXT_SIZE = 10;
    CrcErrorContext _crcContexts[CRC_CONTEXT_SIZE];
    size_t _crcContextIndex{0};
    uint32_t _crcContextNextId{1};
    bool _crcContextPendingNext{false};
    size_t _crcContextPendingIndex{0};

public:
    /**
     * @brief Get recent RX frames for debugging (valid and invalid, last FRAME_HISTORY_SIZE)
     */
    const std::vector<ModbusFrame>& getRecentFrames() const {
        _recentFramesCache.clear();
        _recentFramesCache.reserve(FRAME_HISTORY_SIZE);
        for (size_t i = 0; i < FRAME_HISTORY_SIZE; i++) {
            size_t idx = (_frameHistoryIndex + i) % FRAME_HISTORY_SIZE;
            const ModbusFrame& f = _frameHistory[idx];
            if (f.timestamp == 0) continue;
            _recentFramesCache.push_back(f);
        }
        return _recentFramesCache;
    }
};

#endif // MODBUS_RTU_FEATURE_H
