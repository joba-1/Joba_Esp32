#include "GapPredictor.h"
#include <Arduino.h>
#include <cmath>
#include <algorithm>
#include "LoggingFeature.h"

GapPredictor::GapPredictor() {
    _stats.reset();
    _stats.startMs = millis();
}

void GapPredictor::reset() {
    _busTransitions.clear();
    _globalMinGapMs = UINT32_MAX;
    _stats.reset();
    _stats.startMs = millis();
}

void GapPredictor::recordTransition(uint64_t predecessorKey, uint64_t successorKey, uint32_t gapMs) {
    if (predecessorKey == 0) return;

    // Cap number of predecessors and successors similarly to original logic
    static constexpr size_t MAX_BUS_PATTERNS = 64;

    auto it = _busTransitions.find(predecessorKey);
    if (it != _busTransitions.end()) {
        // Predecessor exists
        if (it->second.size() < MAX_BUS_PATTERNS || it->second.count(successorKey)) {
            it->second[successorKey].record(gapMs);
        }
    } else if (_busTransitions.size() < MAX_BUS_PATTERNS) {
        _busTransitions[predecessorKey][successorKey].record(gapMs);
    }

    if (gapMs < _globalMinGapMs) _globalMinGapMs = gapMs;
}

GapPrediction GapPredictor::predictCurrentGap(uint64_t predecessorKey) const {
    GapPrediction result;
    if (predecessorKey == 0) return result;

    auto predIt = _busTransitions.find(predecessorKey);
    if (predIt == _busTransitions.end()) return result;
    const auto& successors = predIt->second;
    if (successors.empty()) return result;

    uint32_t totalSamples = 0;
    uint32_t usableEdges = 0;
    for (const auto& kv : successors) {
        if (kv.second.count >= 2) {
            totalSamples += kv.second.count;
            usableEdges++;
        }
    }
    if (totalSamples < GAP_MIN_SAMPLES || usableEdges == 0) return result;

    const BusTransitionEntry* bestEdge = nullptr;
    uint32_t bestCount = 0;
    uint32_t hardMinObserved = UINT32_MAX;
    uint32_t minConservativeAll = UINT32_MAX;

    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        if (te.count < 2) continue;

        double mean = te.gapSum / te.count;
        double variance = (te.gapSumSq / te.count) - (mean * mean);
        double stddev = (variance > 0) ? sqrt(variance) : 0;

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

    if (!bestEdge) return result;

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
    return result;
}

bool GapPredictor::canSafelyTransmit(uint64_t predecessorKey, uint32_t wireMs, uint32_t elapsedMs) const {
    if (predecessorKey == 0) return false;

    auto predIt = _busTransitions.find(predecessorKey);
    if (predIt == _busTransitions.end()) return false;
    const auto& successors = predIt->second;
    if (successors.empty()) return false;

    if (_globalMinGapMs != UINT32_MAX && elapsedMs < _globalMinGapMs) return false;

    static constexpr uint32_t FIXED_BUFFER_MS = 15;
    uint32_t safeWireMs = wireMs + FIXED_BUFFER_MS;
    static constexpr uint32_t CONFIDENCE_K = 5;

    bool hasAnyEdge = false;
    bool atLeastOneNotRuledOut = false;
    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        hasAnyEdge = true;

        uint32_t effectiveMin = (uint32_t)((uint64_t)te.gapMin * te.count / (te.count + CONFIDENCE_K));

        if (elapsedMs >= te.gapMin) continue;
        atLeastOneNotRuledOut = true;

        if (elapsedMs + safeWireMs > effectiveMin) {
            return false;
        }
    }

    if (!atLeastOneNotRuledOut) return false;
    return hasAnyEdge;
}

void GapPredictor::reportCollision(bool sentDuringGapWindow, uint32_t lastTxElapsedMs, uint32_t lastTxWireMs,
                                   bool hasLastCompletedTx, uint64_t lastCompletedTxKey) {
    _stats.collisions++;
    _stats.gapInsufficient++;
    _stats.lastCollisionMs = millis();

    LOG_W("COLLISION: sentInGap=%s txElapsed=%ums txWire=%ums globalMin=%ums",
          sentDuringGapWindow ? "yes" : "no", lastTxElapsedMs, lastTxWireMs,
          _globalMinGapMs != UINT32_MAX ? _globalMinGapMs : 0);

    if (hasLastCompletedTx) {
        auto predIt = _busTransitions.find(lastCompletedTxKey);
        if (predIt != _busTransitions.end()) {
            for (const auto& kv : predIt->second) {
                const BusTransitionEntry& te = kv.second;
                LOG_W("  successor gapMin=%u count=%u mean=%.0f",
                      te.gapMin, te.count, te.count > 0 ? te.gapSum / te.count : 0.0);
            }
        }
    }

    float oldMargin = _stats.safetyMargin;
    _stats.safetyMargin = std::min(_stats.safetyMargin + 0.05f, _stats.maxMargin);
    if (_stats.safetyMargin != oldMargin) {
        _stats.lastMarginAdjustMs = millis();
        LOG_W("Gap scheduler: margin %.0f%% -> %.0f%%",
              oldMargin * 100, _stats.safetyMargin * 100);
    }
}
