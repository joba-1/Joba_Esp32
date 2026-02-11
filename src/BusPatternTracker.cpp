#include "BusPatternTracker.h"
#include <algorithm>

#include "ModbusRTUFeature.h"
#include "LoggingFeature.h"

static inline uint64_t makeBusPatternKey(uint8_t unitId, uint8_t fc, uint16_t startReg, uint16_t qty) {
    return ((uint64_t)unitId << 40) | ((uint64_t)fc << 32) | ((uint64_t)startReg << 16) | qty;
}

BusPatternTracker::BusPatternTracker() {
    reset();
}

void BusPatternTracker::reset() {
    _busPatterns.clear();
    _detectedCycle.clear();
    _cycleStepGaps.clear();
    _cycleSeqIndex = 0;
    _cycleSeqCount = 0;
    _cycleTrackingPos = -1;
}

RecordResult BusPatternTracker::recordFrame(const ModbusFrame& frame,
                                            uint32_t minInterframeMs,
                                            bool hasLastCompletedTx,
                                            uint64_t lastCompletedTxKey,
                                            unsigned long lastTransactionEndMs) {
    RecordResult res;
    if (!frame.isRequest || !frame.isValid) return res;

    uint8_t fc = frame.functionCode & 0x7F;
    if (fc != ModbusFC::READ_HOLDING_REGISTERS && fc != ModbusFC::READ_INPUT_REGISTERS) return res;
    if (frame.dataLen < 4) return res;

    uint16_t startReg = frame.getStartRegister();
    uint16_t qty      = frame.getQuantity();
    uint64_t key      = makeBusPatternKey(frame.unitId, fc, startReg, qty);

    auto it = _busPatterns.find(key);
    if (it == _busPatterns.end()) {
        if (_busPatterns.size() >= MAX_BUS_PATTERNS) return res;
        BusPatternEntry entry;
        entry.unitId        = frame.unitId;
        entry.functionCode  = fc;
        entry.startRegister = startReg;
        entry.quantity      = qty;
        entry.count         = 1;
        entry.firstSeenMs   = frame.timestamp;
        entry.lastSeenMs    = frame.timestamp;
        _busPatterns[key] = entry;
    } else {
        BusPatternEntry& e = it->second;
        if (e.lastSeenMs != 0 && frame.timestamp > e.lastSeenMs) {
            uint32_t interval = (uint32_t)(frame.timestamp - e.lastSeenMs);
            e.intervalCount++;
            e.intervalSum   += interval;
            e.intervalSumSq += (double)interval * interval;
            if (interval < e.intervalMin) e.intervalMin = interval;
            if (interval > e.intervalMax) e.intervalMax = interval;
        }
        e.count++;
        e.lastSeenMs = frame.timestamp;
    }

    // Record successor gap on the previous completed transaction
    if (hasLastCompletedTx && frame.timestamp > lastTransactionEndMs) {
        uint32_t gapMs = (uint32_t)(frame.timestamp - lastTransactionEndMs);
        if (gapMs >= minInterframeMs) {
            auto predIt = _busPatterns.find(lastCompletedTxKey);
            if (predIt != _busPatterns.end()) {
                predIt->second.recordSuccessorGap(gapMs);
            }
            res.hasTransition = true;
            res.predecessorKey = lastCompletedTxKey;
            res.successorKey = key;
            res.gapMs = gapMs;
            // Notify via callback if set
            if (_transitionCb) {
                _transitionCb(res.predecessorKey, res.successorKey, res.gapMs);
            }
        }
    }

    // Cycle sequence tracking
    _cycleSeq[_cycleSeqIndex] = key;
    _cycleSeqIndex = (_cycleSeqIndex + 1) % CYCLE_SEQ_SIZE;
    _cycleSeqCount++;

    // Cycle position tracking for gap prediction
    if (!_detectedCycle.empty()) {
        if (_cycleTrackingPos < 0) {
            for (size_t ci = 0; ci < _detectedCycle.size(); ci++) {
                const BusCycleEntry& ce = _detectedCycle[ci];
                if (ce.unitId == frame.unitId && ce.functionCode == fc &&
                    ce.startRegister == startReg && ce.quantity == qty) {
                    _cycleTrackingPos = (int)((ci + 1) % _detectedCycle.size());
                    break;
                }
            }
        } else {
            size_t pos = (size_t)_cycleTrackingPos;
            const BusCycleEntry& expected = _detectedCycle[pos];
            uint64_t expectedKey = makeBusPatternKey(expected.unitId, expected.functionCode,
                                                      expected.startRegister, expected.quantity);
            if (expectedKey == key) {
                if (frame.timestamp > lastTransactionEndMs) {
                    uint32_t gapMs = (uint32_t)(frame.timestamp - lastTransactionEndMs);
                    if (gapMs >= minInterframeMs && pos < _cycleStepGaps.size()) {
                        _cycleStepGaps[pos].record(gapMs);
                    }
                }
                _cycleTrackingPos = (int)((pos + 1) % _detectedCycle.size());
            } else {
                bool found = false;
                for (size_t search = 1; search < _detectedCycle.size(); search++) {
                    size_t tryPos = (pos + search) % _detectedCycle.size();
                    const BusCycleEntry& ce = _detectedCycle[tryPos];
                    if (ce.unitId == frame.unitId && ce.functionCode == fc &&
                        ce.startRegister == startReg && ce.quantity == qty) {
                        _cycleTrackingPos = (int)((tryPos + 1) % _detectedCycle.size());
                        found = true;
                        break;
                    }
                }
                if (!found) _cycleTrackingPos = -1;
            }
        }
    }

    return res;
}

void BusPatternTracker::detectCycle() {
    _detectedCycle.clear();

    size_t available = (_cycleSeqCount < CYCLE_SEQ_SIZE) ? _cycleSeqCount : CYCLE_SEQ_SIZE;
    if (available < 4) return;

    uint64_t seq[CYCLE_SEQ_SIZE];
    if (_cycleSeqCount >= CYCLE_SEQ_SIZE) {
        for (size_t i = 0; i < CYCLE_SEQ_SIZE; ++i) {
            seq[i] = _cycleSeq[(_cycleSeqIndex + i) % CYCLE_SEQ_SIZE];
        }
    } else {
        for (size_t i = 0; i < _cycleSeqCount; ++i) {
            seq[i] = _cycleSeq[i];
        }
    }

    size_t bestLen = 0;
    size_t bestMatches = 0;
    for (size_t tryLen = 1; tryLen <= available / 2 && tryLen <= 64; ++tryLen) {
        size_t matches = 0;
        for (size_t i = tryLen; i < available; ++i) {
            if (seq[i] == seq[i % tryLen]) matches++;
        }
        size_t compared = available - tryLen;
        if (compared > 0 && matches * 100 / compared > 80) {
            if (matches > bestMatches || (matches == bestMatches && tryLen < bestLen)) {
                bestLen = tryLen;
                bestMatches = matches;
            }
        }
    }

    if (bestLen > 0) {
        _detectedCycle.reserve(bestLen);
        for (size_t i = 0; i < bestLen; ++i) {
            uint64_t k = seq[i];
            BusCycleEntry ce;
            ce.unitId        = (uint8_t)(k >> 40);
            ce.functionCode  = (uint8_t)(k >> 32);
            ce.startRegister = (uint16_t)(k >> 16);
            ce.quantity      = (uint16_t)(k & 0xFFFF);
            _detectedCycle.push_back(ce);
        }
        size_t compared = available - bestLen;
        size_t matchPct = (compared > 0) ? (bestMatches * 100 / compared) : 0;
        LOG_I("Bus cycle detected: length=%u, match=%u%% (%u/%u)",
              (unsigned)bestLen, (unsigned)matchPct,
              (unsigned)bestMatches, (unsigned)compared);

        _cycleStepGaps.assign(_detectedCycle.size(), CycleStepStats{});
        _cycleTrackingPos = -1;
    }
}
