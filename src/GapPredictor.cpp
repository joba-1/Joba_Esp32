/**
 * @file GapPredictor.cpp
 * @brief Implementation of the gap prediction logic used to estimate idle
 * gaps on the bus and safely schedule opportunistic transmissions.
 */

#include "GapPredictor.h"
#include <Arduino.h>
#include <cmath>
#include <algorithm>
#include "LoggingFeature.h"

/**
 * @brief Construct a GapPredictor and initialize runtime stats.
 *
 * Sets the start time for stats gathering using `millis()`.
 */
GapPredictor::GapPredictor() {
    _stats.reset();
    _stats.startMs = millis();
    // Create mutex for thread-safe access from web server tasks
    _mutex = xSemaphoreCreateMutex();
}

/**
 * @brief Reset all learned transition statistics and runtime counters.
 *
 * Clears the internal transition map and resets the scheduler statistics.
 * `startMs` is updated to the current `millis()` value.
 */
void GapPredictor::reset() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _busTransitions.clear();
        _globalMinGapMs = UINT32_MAX;
        _stats.reset();
        _stats.startMs = millis();
        xSemaphoreGive(_mutex);
    } else {
        // fallback without lock (shouldn't normally happen)
        _busTransitions.clear();
        _globalMinGapMs = UINT32_MAX;
        _stats.reset();
        _stats.startMs = millis();
    }
}

/**
 * @brief Record an observed gap between a predecessor and successor event.
 *
 * The method updates per-transition statistics (count, sums, min/max)
 * and a global minimum gap. If stats gathering is disabled or the
 * predecessor key is zero, the call is ignored.
 *
 * @param predecessorKey Encoded predecessor identifier.
 * @param successorKey Encoded successor identifier.
 * @param gapMs Observed gap in milliseconds.
 */
void GapPredictor::recordTransition(uint64_t predecessorKey, uint64_t successorKey, uint32_t gapMs) {
    if (predecessorKey == 0 || !_statsEnabled) return;

    static constexpr size_t MAX_BUS_PATTERNS = 16;

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        auto it = _busTransitions.find(predecessorKey);
        if (it != _busTransitions.end()) {
            if (it->second.size() < MAX_BUS_PATTERNS || it->second.count(successorKey)) {
                it->second[successorKey].record(gapMs);
            }
        } else if (_busTransitions.size() < MAX_BUS_PATTERNS) {
            _busTransitions[predecessorKey][successorKey].record(gapMs);
        }

        if (gapMs < _globalMinGapMs) _globalMinGapMs = gapMs;
        xSemaphoreGive(_mutex);
    } else {
        // best-effort fallback without lock
        auto it = _busTransitions.find(predecessorKey);
        if (it != _busTransitions.end()) {
            if (it->second.size() < MAX_BUS_PATTERNS || it->second.count(successorKey)) {
                it->second[successorKey].record(gapMs);
            }
        } else if (_busTransitions.size() < MAX_BUS_PATTERNS) {
            _busTransitions[predecessorKey][successorKey].record(gapMs);
        }
        if (gapMs < _globalMinGapMs) _globalMinGapMs = gapMs;
    }
}

/**
 * @brief Predict a conservative gap (in ms) after the provided predecessor.
 *
 * Uses per-transition statistics to choose the most representative successor
 * and computes a conservative prediction (mean - stddev) reduced by the
 * configured safety margin. Returns a `GapPrediction` describing the
 * predicted gap, confirmation threshold and sample counts.
 *
 * @param predecessorKey Encoded predecessor identifier.
 * @return GapPrediction Result containing validity and predicted values.
 */
GapPrediction GapPredictor::predictCurrentGap(uint64_t predecessorKey) const {
    GapPrediction result;
    if (predecessorKey == 0) return result;

    // Lock during prediction to avoid concurrent modification from recorder
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return result; // unable to lock - return empty prediction
    }

    auto predIt = _busTransitions.find(predecessorKey);
    if (predIt == _busTransitions.end()) { xSemaphoreGive(_mutex); return result; }
    const auto& successors = predIt->second;
    if (successors.empty()) { xSemaphoreGive(_mutex); return result; }

    // Collect simple sample counts across successor edges. We require a
    // minimum number of samples (GAP_MIN_SAMPLES) distributed across at
    // least one usable edge to produce a stable prediction. This avoids
    // returning predictions from very sparse or noisy observations.
    uint32_t totalSamples = 0;
    uint32_t usableEdges = 0;
    for (const auto& kv : successors) {
        if (kv.second.count >= 2) {
            totalSamples += kv.second.count;
            usableEdges++;
        }
    }
    if (totalSamples < GAP_MIN_SAMPLES || usableEdges == 0) {
        xSemaphoreGive(_mutex);
        return result;
    }

    const BusTransitionEntry* bestEdge = nullptr;
    uint32_t bestCount = 0;
    uint32_t hardMinObserved = UINT32_MAX;
    uint32_t minConservativeAll = UINT32_MAX;

    // Evaluate each successor edge to compute a conservative per-edge
    // estimate. We use mean - stddev as a simple conservative statistic
    // (equivalent to the lower side of one stddev) but ensure the value
    // is never smaller than the observed min for that edge. We track the
    // smallest conservative value across all edges (`minConservativeAll`)
    // and select the most-sampled edge as the representative `bestEdge`.
    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        if (te.count < 2) continue;

        double mean = te.gapSum / te.count;
        double variance = (te.gapSumSq / te.count) - (mean * mean);
        double stddev = (variance > 0) ? sqrt(variance) : 0;

        // Conservative estimate: mean minus one stddev, bounded by observed min
        double conservative = mean - stddev;
        if (conservative < (double)te.gapMin) conservative = (double)te.gapMin;
        if (conservative < 0) conservative = 0;
        uint32_t conservativeMs = (uint32_t)conservative;

        if (conservativeMs < minConservativeAll) minConservativeAll = conservativeMs;
        if (te.count > bestCount) {
            bestCount = te.count;
            bestEdge = &te;
        }
        if (te.gapMin < hardMinObserved) hardMinObserved = te.gapMin;
    }

    if (!bestEdge) { xSemaphoreGive(_mutex); return result; }

    // Compute the conservative prediction from the chosen best edge and
    // apply the runtime safety margin. The safety margin is a fraction
    // (e.g., 0.20 = 20%) that reduces the predicted usable gap to avoid
    // risk of collision. The `confirmation` value is a small threshold
    // based on the minimum conservative estimate across all edges and is
    // used by callers to confirm the prediction at runtime.
    double mean = bestEdge->gapSum / bestEdge->count;
    double variance = (bestEdge->gapSumSq / bestEdge->count) - (mean * mean);
    double stddev = (variance > 0) ? sqrt(variance) : 0;

    double bestConservative = mean - stddev;
    if (bestConservative < (double)bestEdge->gapMin) bestConservative = (double)bestEdge->gapMin;
    if (bestConservative < 0) bestConservative = 0;

    uint32_t predicted = (uint32_t)(bestConservative * (1.0 - (double)_stats.safetyMargin));
    uint32_t confirmation = (minConservativeAll != UINT32_MAX) ? (minConservativeAll + 2) : 0;

    result.valid = true;
    result.predictedGapMs = predicted;
    result.confirmationMs = confirmation;
    result.minObservedMs = hardMinObserved;
    result.sampleCount = totalSamples;
    xSemaphoreGive(_mutex);
    return result;
}

/**
 * @brief Heuristic quick-check if a transmission of `wireMs` fits safely.
 *
 * Considers the global minimum gap, per-edge effective minima and a fixed
 * buffer. The function is conservative and returns false unless there is a
 * reasonable chance the transmission will complete before the next foreign
 * transaction.
 *
 * @param predecessorKey Encoded predecessor identifier.
 * @param wireMs Expected wire time of the planned transmission in ms.
 * @param elapsedMs Milliseconds elapsed since the window opened.
 * @return true if the transmission appears safe to attempt, false otherwise.
 */
bool GapPredictor::canSafelyTransmit(uint64_t predecessorKey, uint32_t wireMs, uint32_t elapsedMs) const {
    if (predecessorKey == 0) return false;

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }

    auto predIt = _busTransitions.find(predecessorKey);
    if (predIt == _busTransitions.end()) { xSemaphoreGive(_mutex); return false; }
    const auto& successors = predIt->second;
    if (successors.empty()) { xSemaphoreGive(_mutex); return false; }

    if (_globalMinGapMs != UINT32_MAX && elapsedMs < _globalMinGapMs) { xSemaphoreGive(_mutex); return false; }

    // Conservative buffer added to planned wire time to account for
    // small timing jitter and processing overhead. The `CONFIDENCE_K`
    // term biases the effective minimum toward observed minima when the
    // sample count is low (a simple shrinkage estimator).
    static constexpr uint32_t FIXED_BUFFER_MS = 15;
    uint32_t safeWireMs = wireMs + FIXED_BUFFER_MS;
    static constexpr uint32_t CONFIDENCE_K = 5;

    bool hasAnyEdge = false;
    bool atLeastOneNotRuledOut = false;
    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        hasAnyEdge = true;

        // Effective minimum is a simple shrinkage toward the observed
        // minimum: when `te.count` is small, the divisor increases and the
        // effectiveMin moves closer to zero; as count grows, effectiveMin
        // approaches te.gapMin. This makes the check more conservative
        // when we have fewer observations.
        uint32_t effectiveMin = (uint32_t)((uint64_t)te.gapMin * te.count / (te.count + CONFIDENCE_K));

        // If elapsed already passed the raw observed minimum, this edge
        // is considered expired for scheduling.
        if (elapsedMs >= te.gapMin) continue;
        atLeastOneNotRuledOut = true;

        // If the remaining window (elapsed + planned safe wire time) would
        // exceed the effective minimum for this successor, it is unsafe.
        if (elapsedMs + safeWireMs > effectiveMin) {
            xSemaphoreGive(_mutex);
            return false;
        }
    }

    if (!atLeastOneNotRuledOut) { xSemaphoreGive(_mutex); return false; }
    xSemaphoreGive(_mutex);
    return hasAnyEdge;
}

/**
 * @brief Record a collision where our transmission overlapped another.
 *
 * Updates collision counters, increases the safety margin and logs a
 * warning with contextual information for diagnostics.
 *
 * @param sentDuringGapWindow true when the TX was sent during a predicted gap
 * @param lastTxElapsedMs milliseconds elapsed since the last TX started
 * @param lastTxWireMs wire-time of the last TX in milliseconds
 * @param hasLastCompletedTx true when a prior completed transaction key is known
 * @param lastCompletedTxKey encoded key of the last completed transaction
 */
void GapPredictor::reportCollision(bool sentDuringGapWindow, uint32_t lastTxElapsedMs, uint32_t lastTxWireMs,
                         bool hasLastCompletedTx, uint64_t lastCompletedTxKey) {
    bool locked = false;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        locked = true;
        _stats.collisions++;
        _stats.gapInsufficient++;
        _stats.lastCollisionMs = millis();
    } else {
        _stats.collisions++;
        _stats.gapInsufficient++;
        _stats.lastCollisionMs = millis();
    }

    LOG_W("COLLISION: sentInGap=%s txElapsed=%ums txWire=%ums globalMin=%ums",
        sentDuringGapWindow ? "yes" : "no", lastTxElapsedMs, lastTxWireMs,
        _globalMinGapMs != UINT32_MAX ? _globalMinGapMs : 0);

    float oldMargin = _stats.safetyMargin;
    _stats.safetyMargin = std::min(_stats.safetyMargin + COLLISION_MARGIN_STEP, _stats.maxMargin);
    if (_stats.safetyMargin != oldMargin) {
        _stats.lastMarginAdjustMs = millis();
        LOG_W("Gap scheduler: margin %.0f%% -> %.0f%%",
                oldMargin * 100, _stats.safetyMargin * 100);
    }

    if (locked) xSemaphoreGive(_mutex);
}

/**
 * @brief Inform the predictor that a gap transmission completed successfully.
 *
 * This allows the scheduler to gradually relax the safety margin to be
 * less conservative after repeated successful gap transmissions.
 */
void GapPredictor::noteGapSuccess() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _stats.gapSufficient++;

        if (_stats.safetyMargin <= _stats.minMargin) { xSemaphoreGive(_mutex); return; }

        float oldMargin = _stats.safetyMargin;
        _stats.safetyMargin = std::max(_stats.minMargin, _stats.safetyMargin - SUCCESS_MARGIN_STEP);
        if ((oldMargin - _stats.safetyMargin) >= COLLISION_MARGIN_STEP) {
            LOG_I("Gap scheduler: margin relaxed %.0f%% -> %.0f%% after %u successful gap TXes",
                  oldMargin * 100.0f, _stats.safetyMargin * 100.0f,
                  _stats.gapSufficient);
        }
        xSemaphoreGive(_mutex);
    } else {
        _stats.gapSufficient++;
    }
}

GapSchedulerStats GapPredictor::snapshotStats() const {
    GapSchedulerStats copy;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        copy = _stats;
        xSemaphoreGive(_mutex);
    } else {
        copy = _stats;
    }
    return copy;
}
