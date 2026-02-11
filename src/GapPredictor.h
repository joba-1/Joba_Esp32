#ifndef GAP_PREDICTOR_H
#define GAP_PREDICTOR_H

#include <map>
#include <cstdint>

// Transition entry: statistics for gap between predecessor -> successor
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

using BusTransitionMap = std::map<uint64_t, std::map<uint64_t, BusTransitionEntry>>;

struct GapPrediction {
    bool    valid{false};
    uint32_t predictedGapMs{0};
    uint32_t confirmationMs{0};
    uint32_t minObservedMs{0};
    uint32_t sampleCount{0};
};

struct GapSchedulerStats {
    uint32_t txInGap{0};
    uint32_t txFallback{0};
    uint32_t txDeferred{0};
    uint32_t predictionsUsed{0};
    uint32_t gapSufficient{0};
    uint32_t gapInsufficient{0};
    uint32_t gapSkippedSmall{0};
    uint32_t collisions{0};
    float    safetyMargin{0.20f};
    float    initialMargin{0.20f};
    float    maxMargin{0.60f};
    float    minMargin{0.10f};
    uint32_t totalWireTimeMs{0};
    uint32_t registersRead{0};
    unsigned long lastTxMs{0};
    unsigned long lastCollisionMs{0};
    unsigned long lastMarginAdjustMs{0};
    unsigned long startMs{0};

    void reset() {
        txInGap = txFallback = txDeferred = 0;
        predictionsUsed = gapSufficient = gapInsufficient = gapSkippedSmall = 0;
        collisions = 0;
        safetyMargin = initialMargin;
        totalWireTimeMs = 0;
        registersRead = 0;
        lastTxMs = lastCollisionMs = lastMarginAdjustMs = 0;
        startMs = 0; // initialized in implementation where millis() is available
    }
};

class GapPredictor {
public:
    GapPredictor();

    void reset();

    // Record a transition (predecessor -> successor) observed with gapMs
    void recordTransition(uint64_t predecessorKey, uint64_t successorKey, uint32_t gapMs);

    // Predict gap after given predecessor key
    GapPrediction predictCurrentGap(uint64_t predecessorKey) const;

    // Check whether a TX of wireMs can safely transmit given elapsedMs since window open
    bool canSafelyTransmit(uint64_t predecessorKey, uint32_t wireMs, uint32_t elapsedMs) const;

    const BusTransitionMap& getBusTransitions() const { return _busTransitions; }
    const GapSchedulerStats& stats() const { return _stats; }
    GapSchedulerStats& stats() { return _stats; }
    uint32_t getGlobalMinGapMs() const { return _globalMinGapMs; }

private:
    BusTransitionMap _busTransitions;
    GapSchedulerStats _stats;
    uint32_t _globalMinGapMs{UINT32_MAX};
    static constexpr uint32_t GAP_MIN_SAMPLES = 10;
};

#endif // GAP_PREDICTOR_H
