#ifndef BUS_PATTERN_TRACKER_H
#define BUS_PATTERN_TRACKER_H

#include <map>
#include <vector>
#include <cstdint>
#include <limits>

// --- Bus pattern types (moved here) ---
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

    // Successor gap stats
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

struct BusCycleEntry {
    uint8_t  unitId;
    uint8_t  functionCode;
    uint16_t startRegister;
    uint16_t quantity;
};

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

struct RecordResult {
    bool hasTransition{false};
    uint64_t predecessorKey{0};
    uint64_t successorKey{0};
    uint32_t gapMs{0};
};

class BusPatternTracker {
public:
    BusPatternTracker();
    void reset();
    void detectCycle();

    // Record a request frame. The caller provides the measurement floor and
    // info about the previous completed transaction so the tracker can
    // compute successor gaps and detect transitions. Returns a RecordResult
    // which the caller may use to forward the transition to GapPredictor.
    RecordResult recordFrame(const ModbusFrame& frame,
                             uint32_t minInterframeMs,
                             bool hasLastCompletedTx,
                             uint64_t lastCompletedTxKey,
                             unsigned long lastTransactionEndMs);

    const BusPatternMap& getBusPatterns() const { return _busPatterns; }
    const std::vector<BusCycleEntry>& getDetectedCycle() const { return _detectedCycle; }
    const std::vector<CycleStepStats>& getCycleStepGaps() const { return _cycleStepGaps; }
    int getCycleTrackingPos() const { return _cycleTrackingPos; }

private:
    static constexpr size_t MAX_BUS_PATTERNS = 64;
    static constexpr size_t CYCLE_SEQ_SIZE = 128;

    BusPatternMap _busPatterns;
    std::vector<BusCycleEntry> _detectedCycle;
    std::vector<CycleStepStats> _cycleStepGaps;
    uint64_t _cycleSeq[CYCLE_SEQ_SIZE];
    size_t _cycleSeqIndex{0};
    size_t _cycleSeqCount{0};
    int _cycleTrackingPos{-1};
};

#endif // BUS_PATTERN_TRACKER_H
