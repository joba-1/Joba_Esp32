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
 * @brief Tracking entry for a specific register request pattern on the bus.
 *
 * Identified by unit+FC+startRegister+quantity.  Stores running statistics
 * (count, min/max/mean interval) without keeping individual timestamps, so
 * memory is O(distinct register ranges) regardless of collection duration.
 */
struct BusPatternEntry {
    uint8_t  unitId;
    uint8_t  functionCode;
    uint16_t startRegister;
    uint16_t quantity;
    uint32_t count{0};               // total requests seen
    unsigned long firstSeenMs{0};
    unsigned long lastSeenMs{0};
    // Running interval statistics (between consecutive requests for same range)
    uint32_t intervalCount{0};
    double   intervalSum{0};
    double   intervalSumSq{0};
    uint32_t intervalMin{UINT32_MAX};
    uint32_t intervalMax{0};

    // --- Successor gap stats ---
    // Gap (ms) between this transaction's response and the NEXT request on the bus.
    // This answers: "how long is the bus idle after this register is polled?"
    uint32_t successorGapCount{0};
    double   successorGapSum{0};
    double   successorGapSumSq{0};
    uint32_t successorGapMin{UINT32_MAX};
    uint32_t successorGapMax{0};

    void recordSuccessorGap(uint32_t gapMs) {
        ++successorGapCount;
        successorGapSum += gapMs;
        successorGapSumSq += (double)gapMs * gapMs;
        if (gapMs < successorGapMin) successorGapMin = gapMs;
        if (gapMs > successorGapMax) successorGapMax = gapMs;
    }
};

/**
 * @brief Transition entry: from pattern A to pattern B, with gap statistics.
 *
 * Tracks both how often a transition occurs and the idle-bus gap (ms)
 * between the predecessor's response and the successor's request.
 * This enables per-transition TX window prediction.
 */
struct BusTransitionEntry {
    uint32_t count{0};
    double   gapSum{0};
    double   gapSumSq{0};
    uint32_t gapMin{UINT32_MAX};
    uint32_t gapMax{0};

    void record(uint32_t gapMs) {
        ++count;
        gapSum += gapMs;
        gapSumSq += (double)gapMs * gapMs;
        if (gapMs < gapMin) gapMin = gapMs;
        if (gapMs > gapMax) gapMax = gapMs;
    }
};

/**
 * @brief Transition map: predecessor key -> (successor key -> transition entry).
 *
 * This creates a weighted Markov chain for the bus polling sequence,
 * where each edge has gap statistics for TX window prediction.
 */
using BusTransitionMap = std::map<uint64_t, std::map<uint64_t, BusTransitionEntry>>;

// ---- Gap-aware TX scheduling ----

/**
 * @brief Predicted gap window after observing a foreign transaction.
 *
 * When we see a foreign response complete, we look up the predecessor in the
 * transition map and compute how much idle time is available before the likely
 * next foreign request.  The TX scheduler uses this to decide whether to send
 * our own request(s) into the gap.
 */
struct GapPrediction {
    bool    valid{false};          // true if we have enough data to predict
    uint32_t predictedGapMs{0};   // most-likely successor gap: conservative(best_edge) * (1 - margin)
    uint32_t confirmationMs{0};   // wait at least this long before transmitting to rule out short-gap successors
    uint32_t minObservedMs{0};    // hard minimum ever observed across all successor edges
    uint32_t sampleCount{0};      // total successor observations for this predecessor
};

/**
 * @brief Statistics for the gap-aware TX scheduler.
 *
 * Tracks prediction accuracy, collision count, and the dynamic safety margin.
 * Exposed via /api/modbus/gap-scheduler for monitoring.
 */
struct GapSchedulerStats {
    // TX decisions
    uint32_t txInGap{0};          // sent into a predicted gap
    uint32_t txFallback{0};       // sent via silence-based fallback (no prediction)
    uint32_t txDeferred{0};       // had request ready but gap too small — deferred

    // Prediction quality
    uint32_t predictionsUsed{0};  // times we used a gap prediction
    uint32_t gapSufficient{0};    // prediction said OK and subsequent TX succeeded
    uint32_t gapInsufficient{0};  // prediction said OK but we got a collision/timeout
    uint32_t gapSkippedSmall{0};  // prediction said gap too small, skipped

    // Collisions: we sent into a gap and a foreign frame appeared before our response
    uint32_t collisions{0};

    // Dynamic margin
    float    safetyMargin{0.20f}; // current margin (starts at 20%, adapts)
    float    initialMargin{0.20f};
    float    maxMargin{0.60f};
    float    minMargin{0.10f};

    // Wire time budget
    uint32_t totalWireTimeMs{0};  // cumulative wire time of our TX

    // Registers read successfully via our own requests
    uint32_t registersRead{0};

    // Timing
    unsigned long lastTxMs{0};
    unsigned long lastCollisionMs{0};
    unsigned long lastMarginAdjustMs{0};
    unsigned long startMs{0};           // millis() when stats began (for regs/sec)

    void reset() {
        txInGap = txFallback = txDeferred = 0;
        predictionsUsed = gapSufficient = gapInsufficient = gapSkippedSmall = 0;
        collisions = 0;
        safetyMargin = initialMargin;
        totalWireTimeMs = 0;
        registersRead = 0;
        lastTxMs = lastCollisionMs = lastMarginAdjustMs = 0;
        startMs = millis();
    }
};

/**
 * @brief Gap histogram: time between consecutive frame boundaries on the bus.
 *
 * A "gap" = silence between the last byte of frame N and the first byte of frame N+1,
 * measured in microseconds at the byte-receive level.  This captures ALL inter-frame
 * silences regardless of whether the flanking frames passed CRC.
 */
struct BusGapStats {
    static constexpr size_t NUM_BUCKETS = 12;
    // Bucket boundaries in microseconds (us):
    //  <1ms, 1-3ms, 3-5ms, 5-10ms, 10-20ms, 20-50ms, 50-100ms, 100-200ms, 200-500ms, 500ms-1s, 1-5s, >=5s
    static constexpr uint32_t kBoundariesUs[NUM_BUCKETS] = {
        1000, 3000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 5000000, UINT32_MAX
    };
    uint32_t buckets[NUM_BUCKETS]{};
    uint32_t count{0};
    double   sumUs{0};
    double   sumSqUs{0};
    uint32_t minUs{UINT32_MAX};
    uint32_t maxUs{0};

    void record(uint32_t gapUs) {
        ++count;
        sumUs += gapUs;
        sumSqUs += (double)gapUs * gapUs;
        if (gapUs < minUs) minUs = gapUs;
        if (gapUs > maxUs) maxUs = gapUs;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (gapUs < kBoundariesUs[i]) { ++buckets[i]; break; }
        }
    }

    void reset() { *this = BusGapStats{}; }
};

/**
 * @brief Raw byte-level bus statistics.
 */
struct BusByteStats {
    uint32_t totalBytes{0};
    uint32_t totalFrameBoundaries{0};  // how many times processReceivedData() was called with data
    uint32_t validFrames{0};
    uint32_t invalidFrames{0};         // CRC or parse failures
    unsigned long startMs{0};
    unsigned long lastUpdateMs{0};

    void reset() {
        *this = BusByteStats{};
        startMs = millis();
        lastUpdateMs = startMs;
    }
};

/**
 * @brief Transaction duration stats: time from request to response (measured in ms).
 *
 * Tracks round-trip times for paired request→response transactions on the bus.
 * This answers "how long does a transaction take?" which determines the minimum
 * inter-frame gap needed to fit our own requests.
 */
struct BusTransactionStats {
    static constexpr size_t NUM_BUCKETS = 8;
    // Bucket boundaries in milliseconds:
    //  <10ms, 10-20ms, 20-50ms, 50-100ms, 100-200ms, 200-500ms, 500ms-1s, >=1s
    static constexpr uint32_t kBoundariesMs[NUM_BUCKETS] = {
        10, 20, 50, 100, 200, 500, 1000, UINT32_MAX
    };
    uint32_t buckets[NUM_BUCKETS]{};
    uint32_t count{0};
    double   sumMs{0};
    double   sumSqMs{0};
    uint32_t minMs{UINT32_MAX};
    uint32_t maxMs{0};

    void record(uint32_t durationMs) {
        ++count;
        sumMs += durationMs;
        sumSqMs += (double)durationMs * durationMs;
        if (durationMs < minMs) minMs = durationMs;
        if (durationMs > maxMs) maxMs = durationMs;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (durationMs < kBoundariesMs[i]) { ++buckets[i]; break; }
        }
    }

    void reset() { *this = BusTransactionStats{}; }
};

/**
 * @brief Cycle entry for detected register polling sequence.
 */
struct BusCycleEntry {
    uint8_t  unitId;
    uint8_t  functionCode;
    uint16_t startRegister;
    uint16_t quantity;
};

/**
 * @brief Per-step gap statistics for the detected polling cycle.
 *
 * For each step in the detected cycle, tracks the gap (idle time)
 * between the previous transaction ending and this step's request starting.
 * This enables predicting when large gaps will occur.
 */
struct CycleStepStats {
    uint32_t count{0};
    double   sumMs{0};
    double   sumSqMs{0};
    uint32_t minMs{UINT32_MAX};
    uint32_t maxMs{0};

    void record(uint32_t gapMs) {
        ++count;
        sumMs += gapMs;
        sumSqMs += (double)gapMs * gapMs;
        if (gapMs < minMs) minMs = gapMs;
        if (gapMs > maxMs) maxMs = gapMs;
    }
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
    const std::map<uint64_t, BusPatternEntry>& getBusPatterns() const { return _busPatterns; }

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
    const std::vector<BusCycleEntry>& getDetectedCycle() const { return _detectedCycle; }

    /**
     * @brief Get transaction time statistics (request→response duration)
     */
    const BusTransactionStats& getTransactionStats() const { return _busTransactionStats; }

    /**
     * @brief Get per-step gap statistics for the detected cycle
     */
    const std::vector<CycleStepStats>& getCycleStepGaps() const { return _cycleStepGaps; }

    /**
     * @brief Get current position in detected cycle (-1 = not synced)
     */
    int getCycleTrackingPos() const { return _cycleTrackingPos; }

    /**
     * @brief Get transition map (predecessor -> successor -> count)
     */
    const BusTransitionMap& getBusTransitions() const { return _busTransitions; }
    uint32_t getGlobalMinGapMs() const { return _globalMinGapMs; }

    /**
     * @brief Get gap scheduler stats for monitoring
     */
    const GapSchedulerStats& getGapSchedulerStats() const { return _gapSchedulerStats; }

    /**
     * @brief Predict the available gap after the most recently completed foreign transaction.
     *
     * Looks up the last completed transaction key in the transition map and returns
     * the conservative predicted gap (most-likely successor, with safety margin).
     */
    GapPrediction predictCurrentGap() const;

    /**
     * @brief Check whether a TX of the given wire time can safely fit in the current gap.
     *
     * Unlike predictCurrentGap() (which returns one number), this method checks
     * ALL successor edges.  A successor is "ruled out" when enough silence has
     * elapsed that its request would have already arrived.  Among the remaining
     * (non-ruled-out) successors, the TX is safe only if all of their
     * conservative gaps still leave room for the full wire time.
     *
     * @param wireMs  Estimated wire time of the request + response
     * @return true if it's safe to transmit now, false if we should defer
     */
    bool canSafelyTransmitInGap(uint32_t wireMs) const;

    /**
     * @brief Estimate wire time for a Modbus read request+response.
     *
     * @param quantity Number of registers to read
     * @return Estimated round-trip wire time in milliseconds
     */
    uint32_t estimateWireTimeMs(uint16_t quantity) const;

    /**
     * @brief Report a collision (foreign frame appeared during our TX window).
     *
     * Called from frame processing when we detect our TX was stepped on.
     * Increases the dynamic safety margin.
     */
    void reportCollision();

    /**
     * @brief Reset bus pattern tracking data
     */
    void resetBusPatterns();

    /**
     * @brief Record a request frame into bus pattern tracking
     */
    void recordBusPattern(const ModbusFrame& frame);

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
    void processReceivedData();
    bool parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame);
    void recordFrameToHistory(const ModbusFrame& frame);
    void recordCrcErrorContext(const ModbusFrame& badFrame);
    ModbusRegisterMap& ensureRegisterMap(uint8_t unitId, uint8_t functionCode);
    void updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response);
    void processQueue(bool busSilent = false);
    bool sendRequest(const ModbusPendingRequest& request);
    bool sendFrameFromBuffer();  // Uses static _txFrameBuffer; returns false if aborted
    void sendFrame(const std::vector<uint8_t>& frame);  // Legacy wrapper for sendRawFrame
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
    
    // Multi-master arbitration: wait for foreign response before transmitting
    bool _sawForeignRequest{false};         // True if we saw a request from another master
    unsigned long _foreignRequestTimeMs{0}; // When we saw it
    static constexpr uint32_t FOREIGN_RESPONSE_TIMEOUT_MS = 200; // Max time to wait for their response
    
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
    
    static const size_t FRAME_HISTORY_SIZE = 20;
    ModbusFrame _frameHistory[FRAME_HISTORY_SIZE];
    size_t _frameHistoryIndex = 0;

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
    static uint64_t makeBusPatternKey(uint8_t unitId, uint8_t fc, uint16_t startReg, uint16_t qty) {
        return ((uint64_t)unitId << 40) | ((uint64_t)fc << 32) | ((uint64_t)startReg << 16) | qty;
    }
    std::map<uint64_t, BusPatternEntry> _busPatterns;  // key -> entry
    BusGapStats _busGapStats;               // inter-frame gaps measured at byte level
    BusByteStats _busByteStats;             // raw byte-level diagnostics
    unsigned long _lastFrameBoundaryUs{0};  // micros() of last byte of previous frame chunk
    bool _hasLastFrameBoundary{false};
    std::vector<BusCycleEntry> _detectedCycle;
    // Cycle detection ring buffer: last N request keys in order
    static constexpr size_t CYCLE_SEQ_SIZE = 128;
    uint64_t _cycleSeq[CYCLE_SEQ_SIZE]{};
    size_t _cycleSeqIndex{0};
    size_t _cycleSeqCount{0};              // total entries written (capped display)
    BusTransactionStats _busTransactionStats;  // request→response round-trip times
    std::vector<CycleStepStats> _cycleStepGaps;  // parallel to _detectedCycle
    int _cycleTrackingPos{-1};                    // -1 = not synced to cycle
    unsigned long _lastTransactionEndMs{0};
    bool _hasLastTransactionEnd{false};

    // Successor gap + transition tracking
    BusTransitionMap _busTransitions;    // predecessor key -> successor key -> count
    uint64_t _lastCompletedTxKey{0};     // pattern key of the last completed (request→response) transaction
    bool _hasLastCompletedTx{false};     // true once first transaction is completed
    uint32_t _globalMinGapMs{UINT32_MAX}; // minimum gap observed across ALL transitions

    // Gap-aware TX scheduler state
    GapSchedulerStats _gapSchedulerStats;
    unsigned long _gapWindowOpenMs{0};   // millis() when the current gap window opened
    bool _gapWindowActive{false};        // true if we're in a predicted gap window
    uint32_t _gapWindowBudgetMs{0};      // predicted available ms in this window
    uint32_t _gapWindowUsedMs{0};        // wire time already consumed in this window
    bool _sentDuringGapWindow{false};     // true if current pending request was sent using gap prediction
    uint32_t _lastTxElapsedMs{0};         // gap elapsed at TX time (for collision diagnostics)
    uint32_t _lastTxWireMs{0};            // estimated wire time at TX time
    static constexpr uint32_t GAP_MIN_SAMPLES = 10;  // minimum transition samples before trusting prediction
    static constexpr uint32_t GAP_MIN_USABLE_MS = 20; // minimum usable gap to attempt TX
    static constexpr uint32_t GAP_RELAX_INTERVAL = 50; // relax margin after this many successful gap TXes

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
