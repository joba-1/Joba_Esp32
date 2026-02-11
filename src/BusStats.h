#ifndef BUS_STATS_H
#define BUS_STATS_H

#include <cstdint>
#include <cstddef>
#include <limits>
#include <Arduino.h>

struct BusGapStats {
    static constexpr size_t NUM_BUCKETS = 12;
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

struct BusByteStats {
    uint32_t totalBytes{0};
    uint32_t totalFrameBoundaries{0};
    uint32_t validFrames{0};
    uint32_t invalidFrames{0};
    unsigned long startMs{0};
    unsigned long lastUpdateMs{0};

    void reset() {
        *this = BusByteStats{};
        startMs = millis();
        lastUpdateMs = startMs;
    }
};

struct BusTransactionStats {
    static constexpr size_t NUM_BUCKETS = 8;
    static constexpr uint32_t kBoundariesMs[NUM_BUCKETS] = {10,20,50,100,200,500,1000,UINT32_MAX};
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

#endif // BUS_STATS_H
