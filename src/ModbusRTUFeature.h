#ifndef MODBUS_RTU_FEATURE_H
#define MODBUS_RTU_FEATURE_H

#include <Arduino.h>
#include "ISerial.h"
#include <array>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include "Feature.h"
#include "LoggingFeature.h"
#include "GapPredictor.h"
#include "BusStats.h"

/**
 * @brief Modbus function codes
 */
#include "ModbusFC.h"

#include "ModbusFrame.h"

/**
 * @brief Raw register data storage for a unit/function code combination
 */
#include "ModbusRegisterMap.h"

#include "BusPatternTracker.h"

 

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
    ModbusRTUFeature(ISerial& serial,
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
    
    /**
     * @brief Set silence time for testing different arbitration thresholds
     * @param us Silence time in microseconds (0 = reset to default 3.5 char times)
     */
    void setSilenceTimeUs(uint32_t us) {
        if (us == 0) {
            // Reset to default
            _silenceTimeUs = (_baudRate > 19200) ? 1750 : (_charTimeUs * 35 / 10);
        } else {
            _silenceTimeUs = us;
        }
        LOG_I("Silence time set to %lu us (%.2f char times)", _silenceTimeUs, (float)_silenceTimeUs / _charTimeUs);
    }
    
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

    // ========================================
    // Bus Pattern Analysis
    // ========================================

    /**
     * @brief Get bus pattern entries (per register-range timing stats)
     */
    const std::map<uint64_t, BusPatternEntry>& getBusPatterns() const { return _patternTracker.getBusPatterns(); }

    /**
     * @brief Get inter-frame gap statistics (measured at byte level)
     */
    const BusGapStats& getBusGapStats() const { return _busGapStats; }

    /**
     * @brief Get raw byte-level bus stats
     */
    const BusByteStats& getBusByteStats() const { return _busByteStats; }

    /**
     * @brief Get detected polling cycle (ordered register sequence)
     */
    const std::vector<BusCycleEntry>& getDetectedCycle() const { return _patternTracker.getDetectedCycle(); }

    /**
     * @brief Get transaction time statistics (request→response duration)
     */
    const BusTransactionStats& getTransactionStats() const { return _busTransactionStats; }

    /**
     * @brief Get per-step gap statistics for the detected cycle
     */
    const std::vector<CycleStepStats>& getCycleStepGaps() const { return _patternTracker.getCycleStepGaps(); }

    /**
     * @brief Get current position in detected cycle (-1 = not synced)
     */
    int getCycleTrackingPos() const { return _patternTracker.getCycleTrackingPos(); }

    /**
     * @brief Get transition map (predecessor -> successor -> count)
     */
    const BusTransitionMap& getBusTransitions() const { return _gapPredictor.getBusTransitions(); }
    uint32_t getGlobalMinGapMs() const { return _gapPredictor.getGlobalMinGapMs(); }

    /**
     * @brief Get gap scheduler stats for monitoring
     */
    const GapSchedulerStats& getGapSchedulerStats() const { return _gapPredictor.stats(); }

    /**
     * @brief Get distinct register quantities requested by foreign masters
     */
    const std::set<uint16_t>& getForeignRequestQuantities() const { return _foreignRequestQuantities; }

    /**
     * @brief Predict the available gap after the most recently completed foreign transaction.
     *
     * Looks up the last completed transaction key in the transition map and returns
     * the conservative predicted gap (most-likely successor, with safety margin).
     */
    GapPrediction predictCurrentGap() const;

    /**
     * @brief Estimate wire time for a Modbus read request+response.
     *
     * @param quantity Number of registers to read
     * @return Estimated round-trip wire time in milliseconds
     */
    uint32_t estimateWireTimeMs(uint16_t quantity) const;

    /**
     * @brief Reset bus pattern tracking data
     */
    void resetBusPatterns();

    
    /**
     * @brief Record an inter-frame gap (in microseconds)
     */
    void recordBusGap(uint32_t gapUs);

    /**
     * @brief Called at frame boundary (processReceivedData entry) to track byte stats and gaps
     */
    void onFrameBoundary(size_t bytesInBuffer);

    /**
     * @brief Force cycle detection from current pattern data
     */
    void detectCycle();

private:
    /**
     * @brief Read incoming serial data, buffer it and process complete frames.
     *
     * This is the top-level RX handler invoked from `loop()`; it extracts
     * complete frames and dispatches them to parsing/handling helpers.
     */
    void processReceivedData();
    // Helpers extracted from the large processReceivedData() function
    /**
     * @brief Attempt to extract complete frames from the RX buffer.
     * @return Number of frames extracted.
     */
    size_t extractFramesFromRxBuffer();

    /**
     * @brief Handle a frame that has been parsed from the RX buffer.
     * @param frame Parsed ModbusFrame structure.
     * @param isRequest True if the frame is a request, false if response.
     * @param frameLen Length of the raw frame in bytes.
     */
    void handleParsedFrame(const ModbusFrame& frame, bool isRequest, size_t frameLen);
    // Further decomposition helpers (small, focused units)
    /**
     * @brief Try to parse a frame at a specific length from buffer pointer `p`.
     * @return True on successful parse and CRC check.
     */
    bool tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out);

    /**
     * @brief Determine expected frame length from the current buffer content.
     * @param p Pointer to buffer start, remaining bytes available.
     * @param remaining Number of bytes available at `p`.
     * @param isRequest Output flag set to true if parsed frame appears to be a request.
     * @param frameLen Output length in bytes if detected.
     * @return True if a plausible length could be determined.
     */
    bool determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen);

    /**
     * @brief Parse raw bytes at `offset` and compute metadata (timestamps, CRC, unit/fc).
     */
    void parseFrameAndComputeMetadata(size_t offset, size_t frameLen, ModbusFrame& frame, bool isRequest, uint32_t approxStartMs);

    /**
     * @brief Handle CRC-invalid frames and attempt resynchronization when allowed.
     */
    void handleCrcInvalidFrame(const ModbusFrame& frame, bool resyncAllowed);

    /**
     * @brief Handle a response that originated from this device's own request.
     */
    void handleOurResponse(const ModbusFrame& frame, size_t frameLen);

    /**
     * @brief Handle a request sent by another master on the bus.
     */
    void handleForeignRequest(const ModbusFrame& frame);

    /**
     * @brief Handle a response sent by another device (not our request).
     */
    void handleForeignResponse(const ModbusFrame& frame, size_t frameLen);

    /**
     * @brief Finish processing of a frame and notify registered callbacks.
     */
    void finishFrameProcessingAndNotify(const ModbusFrame& frame, bool isRequest);

    /**
     * @brief Advance scan index through RX buffer and return number of bytes consumed.
     */
    size_t scanAndAdvanceIndex();

    /**
     * @brief Parse a raw Modbus frame from `data` into `frame`.
     */
    bool parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame);

    /**
     * @brief Append `frame` into the recent frame history ring buffer.
     */
    void recordFrameToHistory(const ModbusFrame& frame);

    /**
     * @brief Record CRC error context for diagnostics.
     */
    void recordCrcErrorContext(const ModbusFrame& badFrame);

    /**
     * @brief Ensure a ModbusRegisterMap exists for `unitId`/`functionCode` and return it.
     */
    ModbusRegisterMap& ensureRegisterMap(uint8_t unitId, uint8_t functionCode);

    /**
     * @brief Update stored register map based on request/response pair.
     */
    void updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response);

    /**
     * @brief Process the outgoing request queue, optionally when bus is silent.
     */
    void processQueue(bool busSilent = false);

    /**
     * @brief Send the provided pending request onto the bus, return true if transmitted.
     */
    bool sendRequest(const ModbusPendingRequest& request);

    /**
     * @brief Attempt to send the already-prepared frame buffer; returns false if aborted.
     */
    bool sendFrameFromBuffer();  // Uses static _txFrameBuffer; returns false if aborted

    /**
     * @brief Send a vector-backed raw frame (legacy wrapper around sendRawFrame).
     */
    void sendFrame(const std::vector<uint8_t>& frame);  // Legacy wrapper for sendRawFrame

    /**
     * @brief Calculate CRC16 for `data` of `length` bytes.
     */
    uint16_t calculateCRC(const uint8_t* data, size_t length) const;

    /**
     * @brief Drive DE/RE pin to enable/disable RS485 transmitter.
     */
    void setDE(bool transmit);

    /**
     * @brief Check internal warning thresholds and log if exceeded.
     */
    void checkAndLogWarnings();

    /**
     * @brief Mark start of active communication interval (own or other).
     */
    void startActiveTime(bool isOwn);

    /**
     * @brief Mark end of active communication interval.
     */
    void endActiveTime();
    
    /**
     * @brief Record a request frame into bus pattern tracking.
     *
     * Extracts pattern key from the `ModbusFrame` and updates per-pattern
     * counts and successor-gap statistics (delegates to `GapPredictor`).
     */
    void recordBusPattern(const ModbusFrame& frame);
    
    static uint16_t makeMapKey(uint8_t unitId, uint8_t functionCode) {
        return (unitId << 8) | functionCode;
    }
    
    ISerial& _serial;
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
    
    // Multi-master arbitration: wait for foreign response before transmitting
    bool _sawForeignRequest{false};         // True if we saw a request from another master
    unsigned long _foreignRequestTimeMs{0}; // When we saw it
    static constexpr uint32_t FOREIGN_RESPONSE_TIMEOUT_MS = 200; // Max time to wait for their response
    
    // Frame tracking for request/response matching
    ModbusFrame _lastRequest;
    std::map<uint8_t, ModbusFrame> _lastRequestPerUnit;
    std::set<uint16_t> _foreignRequestQuantities;  // Distinct register quantities seen from foreign masters
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
        uint32_t backoffMs{1000};  // Initial backoff: 1 second (will double on subsequent timeouts)
        uint32_t pausedUntilMs{0};
        uint32_t lastProbeAttemptMs{0};  // Track last probe attempt when bus was silent
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

    // Static TX frame buffer (avoids heap allocation per send)
    // Max Modbus RTU frame: 256 bytes payload + 3 header + 2 CRC = 261
    static constexpr size_t TX_FRAME_BUFFER_SIZE = 264;
    uint8_t _txFrameBuffer[TX_FRAME_BUFFER_SIZE];
    size_t _txFrameLen{0};

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
    
    static const size_t FRAME_HISTORY_SIZE = 10;
    ModbusFrame _frameHistory[FRAME_HISTORY_SIZE];
    size_t _frameHistoryIndex = 0;

    // RX buffer timing (best-effort start-of-buffer timestamps)
    uint32_t _rxBufferStartUs{0};
    uint32_t _rxBufferStartMs{0};

    // CRC error contexts (before/bad/after)
    static const size_t CRC_CONTEXT_SIZE = 4;
    CrcErrorContext _crcContexts[CRC_CONTEXT_SIZE];
    size_t _crcContextIndex{0};
    uint32_t _crcContextNextId{1};
    bool _crcContextPendingNext{false};
    size_t _crcContextPendingIndex{0};

    // Frame resynchronization state (persists across processReceivedData() calls)
    bool _inResync{false};

    // Response mismatch tracking (for diagnostics)
    struct ResponseMismatch {
        unsigned long timestamp;
        uint8_t expectedUnit;
        uint8_t actualUnit;
        uint8_t expectedFc;
        uint8_t actualFc;
        bool byteCountMatch;
    };
    static const size_t MISMATCH_HISTORY_SIZE = 10;
    ResponseMismatch _mismatchHistory[MISMATCH_HISTORY_SIZE];
    size_t _mismatchIndex{0};
    uint32_t _mismatchCount{0};

    // ---- Bus pattern analysis ----
    BusGapStats _busGapStats;               // inter-frame gaps measured at byte level
    BusByteStats _busByteStats;             // raw byte-level diagnostics
    unsigned long _lastFrameBoundaryUs{0};  // micros() of last byte of previous frame chunk
    bool _hasLastFrameBoundary{false};

    BusPatternTracker _patternTracker;

    
    BusTransactionStats _busTransactionStats;  // request→response round-trip times
    unsigned long _lastTransactionEndMs{0};
    bool _hasLastTransactionEnd{false};

    // Successor gap + transition tracking (capped: MAX_BUS_PATTERNS outer keys)
    GapPredictor _gapPredictor;
    uint64_t _lastCompletedTxKey{0};     // pattern key of the last completed (request→response) transaction
    bool _hasLastCompletedTx{false};     // true once first transaction is completed
    // Gap-aware TX scheduler state
    // scheduler stats are available via `_gapPredictor.stats()`
    unsigned long _gapWindowOpenMs{0};   // millis() when the current gap window opened
    bool _gapWindowActive{false};        // true if we're in a predicted gap window
    uint32_t _gapWindowBudgetMs{0};      // predicted available ms in this window
    uint32_t _gapWindowUsedMs{0};        // wire time already consumed in this window
    bool _sentDuringGapWindow{false};     // true if current pending request was sent using gap prediction
    uint8_t _ownTxInCurrentGap{0};        // consecutive own TXes in current gap (for inter-TX silence reduction)
    uint32_t _lastTxElapsedMs{0};         // gap elapsed at TX time (for collision diagnostics)
    uint32_t _lastTxWireMs{0};            // estimated wire time at TX time
    static constexpr uint32_t GAP_MIN_SAMPLES = 10;  // minimum transition samples before trusting prediction
    static constexpr uint32_t GAP_MIN_USABLE_MS = 20;  // minimum usable gap to attempt TX
    static constexpr uint32_t RESPONSE_MIN_WINDOW_MS = 3;  // min time to accept response (avoid echo)
    static constexpr uint32_t RESPONSE_MAX_WINDOW_MS = 200;  // max time to accept response (strict window)

public:
    /**
     * @brief Get recent RX frames for debugging (valid and invalid, last FRAME_HISTORY_SIZE)
     */
    /**
     * @brief Iterate recent RX frames in chronological order (zero-copy, no heap allocation).
     * Calls fn(const ModbusFrame&) for each valid frame in the ring buffer.
     */
    template<typename Fn>
    void forEachRecentFrame(Fn&& fn) const {
        for (size_t i = 0; i < FRAME_HISTORY_SIZE; i++) {
            size_t idx = (_frameHistoryIndex + i) % FRAME_HISTORY_SIZE;
            const ModbusFrame& f = _frameHistory[idx];
            if (f.timestamp == 0) continue;
            fn(f);
        }
    }
    
    /**
     * @brief Get response mismatch history for diagnostics
     */
    const ResponseMismatch* getMismatchHistory(size_t& outCount, uint32_t& totalMismatches) const {
        outCount = MISMATCH_HISTORY_SIZE;
        totalMismatches = _mismatchCount;
        return _mismatchHistory;
    }
};

#endif // MODBUS_RTU_FEATURE_H
