#ifndef BUS_PATTERN_TRACKER_H
#define BUS_PATTERN_TRACKER_H

#include <map>
#include <vector>
#include <cstdint>
#include <functional>
#include <limits>

/**
 * @file BusPatternTracker.h
 * @brief Track per-register polling patterns and detect polling cycles.
 *
 * `BusPatternTracker` maintains compact statistics for distinct register-range
 * requests observed on the Modbus bus. Each `BusPatternEntry` aggregates
 * occurrence counts and inter-arrival intervals; successor-gap statistics are
 * recorded to enable transition-based gap prediction (GapPredictor consumes
 * recorded transitions).
 */

/**
 * @brief Statistics for a unique register-range polling pattern.
 *
 * Identified by unitId/functionCode/startRegister/quantity. Only aggregated
 * statistics are kept (counts, sums, min/max) to keep memory usage bounded.
 */
struct BusPatternEntry {
    uint8_t  unitId;
    uint8_t  functionCode;
    uint16_t startRegister;
    uint16_t quantity;
    uint32_t count{0};
    unsigned long firstSeenMs{0};
    unsigned long lastSeenMs{0};
    uint32_t intervalCount{0};
    double   intervalSum{0};
    double   intervalSumSq{0};
    uint32_t intervalMin{UINT32_MAX};
    uint32_t intervalMax{0};

    // Successor gap stats: gaps (ms) between this pattern's response and the
    // subsequent request that follows it on the bus.
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
 * @brief Utility: encode a pattern key from identity fields.
 *
 * The implementation file provides a matching decoder. The key is a
 * compact 64-bit value combining `unitId`, `functionCode`, `startRegister`
 * and `quantity` suitable for map lookups. The layout (high->low) is:
 * [unitId:8][functionCode:8][startRegister:16][quantity:16]
 */
static inline uint64_t encodePatternKey(uint8_t unitId, uint8_t functionCode, uint16_t startRegister, uint16_t quantity) {
    return ((uint64_t)unitId << 40) | ((uint64_t)functionCode << 32) | ((uint64_t)startRegister << 16) | (uint64_t)quantity;
}

/**
 * @brief Compact identifier for a cycle step (used when a polling cycle is detected).
 */
struct BusCycleEntry {
    uint8_t  unitId;
    uint8_t  functionCode;
    uint16_t startRegister;
    uint16_t quantity;
};

/**
 * @brief Per-step gap statistics for detected cycles.
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

// Forward-declare ModbusFrame so this header can be included where ModbusFrame is
// already defined; implementation file includes the full ModbusRTUFeature.h.
struct ModbusFrame;

using BusPatternMap = std::map<uint64_t, BusPatternEntry>;

/**
 * @brief Result returned when recording a frame.
 *
 * If a transition was observed (predecessor -> successor), `hasTransition`
 * will be true and `predecessorKey`/`successorKey`/`gapMs` contain the
 * relevant data to forward to `GapPredictor`.
 */
struct RecordResult {
    bool hasTransition{false};
    uint64_t predecessorKey{0};
    uint64_t successorKey{0};
    uint32_t gapMs{0};
};

/**
 * @brief Tracks observed register-range patterns and detects cycles.
 *
 * Usage: call `recordFrame()` for each request frame. The tracker updates
 * per-pattern stats and may return a `RecordResult` containing a discovered
 * transition to feed into `GapPredictor`.
 */
class BusPatternTracker {
public:
    /**
     * @brief Construct a BusPatternTracker and initialize internal state.
     */
    BusPatternTracker();

    /**
     * @brief Clear all tracked patterns and reset internal buffers.
     */
    void reset();

    /**
     * @brief Attempt to detect a repeating polling cycle from recent history.
     *
     * When a cycle is found, `_detectedCycle` and `_cycleStepGaps` are
     * populated with compact descriptions and per-step statistics.
     */
    void detectCycle();

    /**
     * @brief Record a request frame and update pattern statistics.
     *
     * The caller should invoke this for each observed request frame. The
     * tracker updates per-pattern counters, computes inter-arrival
     * intervals and may detect a predecessor->successor transition.
     *
     * @param frame Observed `ModbusFrame` request.
     * @param minInterframeMs Minimum inter-frame threshold (measurement floor)
     *        to filter spurious small gaps.
     * @param hasLastCompletedTx True when information about the last
     *        completed transaction is available.
     * @param lastCompletedTxKey Encoded key of the last completed transaction
     *        (used to locate predecessor statistics).
     * @param lastTransactionEndMs Uptime in ms when the last transaction ended.
     * @return RecordResult Contains `hasTransition==true` when a valid
     *         predecessor->successor transition was observed.
     */
    RecordResult recordFrame(const ModbusFrame& frame,
                             uint32_t minInterframeMs,
                             bool hasLastCompletedTx,
                             uint64_t lastCompletedTxKey,
                             unsigned long lastTransactionEndMs);

    using TransitionCallback = std::function<void(uint64_t predecessorKey, uint64_t successorKey, uint32_t gapMs)>;
    /**
     * @brief Set an optional callback invoked when a transition is discovered.
     *
     * The callback receives `predecessorKey`, `successorKey` and the
     * observed `gapMs` so callers can forward transitions to `GapPredictor`.
     */
    void setTransitionCallback(TransitionCallback cb) { _transitionCb = cb; }

    const BusPatternMap& getBusPatterns() const { return _busPatterns; }
    const std::vector<BusCycleEntry>& getDetectedCycle() const { return _detectedCycle; }
    const std::vector<CycleStepStats>& getCycleStepGaps() const { return _cycleStepGaps; }
    int getCycleTrackingPos() const { return _cycleTrackingPos; }

private:
    static constexpr size_t MAX_BUS_PATTERNS = 32;
    static constexpr size_t CYCLE_SEQ_SIZE = 64;

    BusPatternMap _busPatterns;
    std::vector<BusCycleEntry> _detectedCycle;
    std::vector<CycleStepStats> _cycleStepGaps;
    uint64_t _cycleSeq[CYCLE_SEQ_SIZE];
    size_t _cycleSeqIndex{0};
    size_t _cycleSeqCount{0};
    int _cycleTrackingPos{-1};
    TransitionCallback _transitionCb; // optional callback to notify about discovered transitions
};

#endif // BUS_PATTERN_TRACKER_H
