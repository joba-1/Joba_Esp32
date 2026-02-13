#ifndef GAP_PREDICTOR_H
#define GAP_PREDICTOR_H

#include <map>
#include <cstdint>

/**
 * @file GapPredictor.h
 * @brief Predict idle gaps and manage gap-aware TX scheduling statistics.
 *
 * `GapPredictor` aggregates per-transition gap statistics (predecessor ->
 * successor) so the scheduler can conservatively estimate how many
 * milliseconds are likely available after a foreign transaction completes.
 */

/**
 * @brief Transition entry: statistics for gap between predecessor -> successor
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
 * @brief Record an observed gap sample for this transition.
 *
 * Updates aggregated counters and moment sums used later to compute
 * mean/variance and conservative estimates for scheduling.
 *
 * @param gapMs observed gap in milliseconds
 */

using BusTransitionMap = std::map<uint64_t, std::map<uint64_t, BusTransitionEntry>>;

/**
 * @brief Gap prediction result returned by `predictCurrentGap()`.
 */
struct GapPrediction {
    bool    valid{false};
    uint32_t predictedGapMs{0};
    uint32_t confirmationMs{0};
    uint32_t minObservedMs{0};
    uint32_t sampleCount{0};
};

/**
 * @brief Runtime statistics for the gap scheduler.
 *
 * Tracks how often gap predictions were used, collisions observed and
 * heuristic margins applied to stay conservative when scheduling TX during
 * predicted gaps.
 */
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

/**
 * @brief Reset runtime statistics for the gap scheduler.
 *
 * Clears counters and resets the safety margin back to the configured
 * `initialMargin`. The `startMs` field is set in runtime code using
 * `millis()` where real time is available.
 */

/**
 * @brief Predicts gaps and provides safety checks for transmitting in a gap.
 *
 * The class stores per-transition histograms and exposes:
 * - `recordTransition()` to feed observed predecessor->successor gaps,
 * - `predictCurrentGap()` to get a conservative predicted gap for a given
 *    predecessor, and
 * - `canSafelyTransmit()` for a quick check whether a planned TX fits the
 *    currently available budget.
 */
class GapPredictor {
public:
    /**
     * @brief Construct a GapPredictor and initialize runtime statistics.
     *
     * The implementation initializes the internal `GapSchedulerStats` and
     * records the current `millis()` as the start time.
     */
    GapPredictor();

    static constexpr float COLLISION_MARGIN_STEP = 0.01f;  // +1%
    static constexpr float SUCCESS_MARGIN_STEP = 0.002f;  // -0.2% per success

    /**
     * @brief Clear learned data and reset runtime statistics.
     *
     * Clears the transition map, resets the global minimum gap and
     * reinitializes scheduler statistics. `startMs` will be set using
     * `millis()` in the implementation.
     */
    void reset();

    // Record a transition (predecessor -> successor) observed with gapMs
    /**
     * @brief Feed an observed predecessor -> successor gap into the model.
     *
     * @param predecessorKey Encoded predecessor identifier (non-zero).
     * @param successorKey Encoded successor identifier.
     * @param gapMs Observed gap in milliseconds between transactions.
     */
    void recordTransition(uint64_t predecessorKey, uint64_t successorKey, uint32_t gapMs);

    // Predict gap after given predecessor key
    /**
     * @brief Predict a conservative gap after the given predecessor.
     *
     * Returns a `GapPrediction` structure containing a conservative
     * predicted gap (reduced by safety margin), a confirmation threshold
     * and sample counts. If insufficient data is available the result
     * will have `valid == false`.
     */
    GapPrediction predictCurrentGap(uint64_t predecessorKey) const;

    // Check whether a TX of wireMs can safely transmit given elapsedMs since window open
    /**
     * @brief Quick heuristic to check whether a transmission fits safely.
     *
     * The method uses per-edge minima, a small fixed buffer and the global
     * minimum gap to conservatively decide whether a planned transmission
     * (wireMs) is likely to complete given `elapsedMs` since the window
     * opened.
     */
    bool canSafelyTransmit(uint64_t predecessorKey, uint32_t wireMs, uint32_t elapsedMs) const;

    // Record a collision event (our TX was stepped on). Pass contextual info for logging.
    /**
     * @brief Notify the predictor that our transmission collided with another.
     *
     * Adjusts internal safety margins and records collision timestamps for
     * diagnostic purposes.
     */
    void reportCollision(bool sentDuringGapWindow, uint32_t lastTxElapsedMs, uint32_t lastTxWireMs,
                         bool hasLastCompletedTx, uint64_t lastCompletedTxKey);

    // Record a successful gap TX to relax safety margin over time.
    /**
     * @brief Inform the predictor that a transmission completed successfully
     *        within the predicted gap.
     *
     * This allows the scheduler to slowly relax the safety margin after
     * repeated successful gap transmissions.
     */
    void noteGapSuccess();

    // Enable/disable stats gathering to avoid polluting data during own TX
    /**
     * @brief Enable or disable runtime statistics gathering.
     *
     * Disable when the device is performing its own transmissions to avoid
     * polluting learned statistics about foreign masters.
     */
    void setStatsEnabled(bool enabled) { _statsEnabled = enabled; }
    bool areStatsEnabled() const { return _statsEnabled; }

    const BusTransitionMap& getBusTransitions() const { return _busTransitions; }
    const GapSchedulerStats& stats() const { return _stats; }
    GapSchedulerStats& stats() { return _stats; }
    uint32_t getGlobalMinGapMs() const { return _globalMinGapMs; }

private:
    BusTransitionMap _busTransitions;
    GapSchedulerStats _stats;
    uint32_t _globalMinGapMs{UINT32_MAX};
    bool _statsEnabled{true};  // Disable during own TX to avoid polluting foreign master stats
    static constexpr uint32_t GAP_MIN_SAMPLES = 10;
};

#endif // GAP_PREDICTOR_H
