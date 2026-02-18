#include "ModbusRTUFeature.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include "TimeUtils.h"
#include "modbus_helpers.h"
#include "ModbusRTUFeature_helper.h"
#include "CpuMonitor.h"

static inline bool timeBefore32(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

// Modbus RTU constants
static constexpr uint8_t MAX_RTU_UNIT_ID = 247;
static constexpr size_t MIN_FRAME_SIZE = 4;

// Helper to check if function code is a read operation
static inline bool isReadFunction(uint8_t fc) {
    return (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS);
}

bool ModbusRTUFeature::isQueueingPaused() const {
    if (_requestQueue.empty()) return false;

    // Consider the queue "paused" only if *all* queued requests are currently paused.
    for (const auto& req : _requestQueue) {
        if (!isUnitQueueingPaused(req.unitId)) return false;
    }
    return true;
}

uint32_t ModbusRTUFeature::getQueueingPauseRemainingMs() const {
    if (!isQueueingPaused()) return 0;
    uint32_t minRemaining = 0;
    bool any = false;
    for (const auto& req : _requestQueue) {
        const uint32_t rem = getUnitQueueingPauseRemainingMs(req.unitId);
        if (rem == 0) continue;
        if (!any || rem < minRemaining) {
            minRemaining = rem;
            any = true;
        }
    }
    return any ? minRemaining : 0;
}

uint32_t ModbusRTUFeature::getQueueingPausedUntilMs() const {
    if (!isQueueingPaused()) return 0;
    uint32_t minUntil = 0;
    bool any = false;
    for (const auto& req : _requestQueue) {
        auto it = _backoffByUnit.find(req.unitId);
        if (it == _backoffByUnit.end()) continue;
        const uint32_t until = it->second.pausedUntilMs;
        if (until == 0) continue;
        if (!any || timeBefore32(until, minUntil)) {
            minUntil = until;
            any = true;
        }
    }
    return any ? minUntil : 0;
}

uint32_t ModbusRTUFeature::getQueueingBackoffMs() const {
    // Return the maximum backoff window across units (diagnostics only).
    uint32_t maxBackoff = 0;
    for (const auto& kv : _backoffByUnit) {
        maxBackoff = std::max(maxBackoff, kv.second.backoffMs);
    }
    return maxBackoff;
}

uint32_t ModbusRTUFeature::getConsecutiveTimeouts() const {
    // Return the maximum consecutive timeout streak across units (diagnostics only).
    uint32_t maxTimeouts = 0;
    for (const auto& kv : _backoffByUnit) {
        maxTimeouts = std::max(maxTimeouts, kv.second.consecutiveTimeouts);
    }
    return maxTimeouts;
}

bool ModbusRTUFeature::isUnitQueueingPaused(uint8_t unitId) const {
    auto it = _backoffByUnit.find(unitId);
    if (it == _backoffByUnit.end()) return false;
    const TimeoutBackoffState& st = it->second;
    if (st.consecutiveTimeouts <= 2) return false;
    const uint32_t now = (uint32_t)millis();
    return timeBefore32(now, st.pausedUntilMs);
}

void ModbusRTUFeature::suspend() {
    if (_suspended) return;
    _suspended = true;
    
    // Clear pending requests - they would timeout anyway during OTA
    _requestQueue.clear();
    _waitingForResponse = false;
    _hasPendingRequest = false;
    
    // Drain and discard any buffered RX data
    _rxBuffer.clear();
    while (_serial.available()) {
        _serial.read();
    }
    
    LOG_I("ModbusRTU suspended");
}

/**
 * @brief Resume Modbus processing after a previous suspend.
 *
 * Re-synchronizes timing, clears RX buffers and prepares the driver to
 * resume normal operation.
 */

void ModbusRTUFeature::resume() {
    if (!_suspended) return;
    _suspended = false;
    
    // Re-synchronize timing
    _lastByteTime = micros();
    _lastActivityTime = millis();
    _busSilent = true;
    _serialWasEmpty = true;
    _serialEmptySinceUs = micros();
    
    // Drain any garbage that accumulated
    _rxBuffer.clear();
    while (_serial.available()) {
        _serial.read();
    }
    
    LOG_I("ModbusRTU resumed");
}

/**
 * @brief Perform Modbus RTU feature setup (open serial, configure pins).
 *
 * Initializes UART, DE/RE pin (if configured), internal timing and
 * statistics. Safe to call multiple times; subsequent calls when already
 * ready are no-ops.
 */

uint32_t ModbusRTUFeature::getUnitQueueingPauseRemainingMs(uint8_t unitId) const {
    if (!isUnitQueueingPaused(unitId)) return 0;
    auto it = _backoffByUnit.find(unitId);
    if (it == _backoffByUnit.end()) return 0;
    const uint32_t now = (uint32_t)millis();
    return (uint32_t)(it->second.pausedUntilMs - now);
}

uint32_t ModbusRTUFeature::getUnitQueueingBackoffMs(uint8_t unitId) const {
    auto it = _backoffByUnit.find(unitId);
    if (it == _backoffByUnit.end()) return 2000;
    return it->second.backoffMs;
}

uint32_t ModbusRTUFeature::getUnitConsecutiveTimeouts(uint8_t unitId) const {
    auto it = _backoffByUnit.find(unitId);
    if (it == _backoffByUnit.end()) return 0;
    return it->second.consecutiveTimeouts;
}

std::vector<ModbusRTUFeature::UnitBackoffInfo> ModbusRTUFeature::getUnitBackoffInfo() const {
    std::vector<UnitBackoffInfo> out;
    out.reserve(_backoffByUnit.size());
    const uint32_t now = (uint32_t)millis();

    for (const auto& kv : _backoffByUnit) {
        const uint8_t unitId = kv.first;
        const TimeoutBackoffState& st = kv.second;
        const bool paused = (st.consecutiveTimeouts > 2) && timeBefore32(now, st.pausedUntilMs);
        const uint32_t rem = paused ? (uint32_t)(st.pausedUntilMs - now) : 0;
        out.push_back(UnitBackoffInfo{unitId, st.consecutiveTimeouts, st.backoffMs, st.pausedUntilMs, paused, rem});
    }
    return out;
}

ModbusRTUFeature::ModbusRTUFeature(ISerial& serial,
                                   uint32_t baudRate,
                                   uint32_t config,
                                   int8_t rxPin,
                                   int8_t txPin,
                                   int8_t dePin,
                                   size_t maxQueueSize,
                                   uint32_t responseTimeoutMs)
    : _serial(serial)
    , _baudRate(baudRate)
    , _config(config)
    , _rxPin(rxPin)
    , _txPin(txPin)
    , _dePin(dePin)
    , _maxQueueSize(maxQueueSize)
    , _responseTimeoutMs(responseTimeoutMs)
    , _lastByteTime(0)
    , _lastActivityTime(0)
    , _busSilent(true)
    , _ready(false)
    , _waitingForResponse(false)
    , _requestSentTime(0)
    , _hasPendingRequest(false)
    , _lastSuccessTime(0)
    , _lastTimeoutWarningMs(0)
    , _frameCallback(nullptr)
    , _stats{}
    , _intervalStats{}
    , _inActiveTime(false)
    , _activeTimeIsOwn(false)
    , _activeStartTimeUs(0)
    , _lastWarningCheckMs(0)
{
    // Calculate timing based on baud rate
    // Character time = (start + data + parity + stop) bits / baud
    // For 8N1: 10 bits per character
    uint8_t bitsPerChar = 10;  // 1 start + 8 data + 0 parity + 1 stop
    if (config == SERIAL_8E1 || config == SERIAL_8O1 || config == SERIAL_8N2) bitsPerChar = 11;
    if (config == SERIAL_8E2 || config == SERIAL_8O2) bitsPerChar = 12;
    
    _charTimeUs = (bitsPerChar * 1000000UL) / baudRate;
    
    // Modbus spec: 3.5 character times silence between frames
    // At baud rates > 19200, use fixed 1.75ms
    if (baudRate > 19200) {
        _silenceTimeUs = 1750;
    } else {
        // Modbus RTU spec: 3.5 character times silence between frames
        _silenceTimeUs = _charTimeUs * 35 / 10;  // 3.5 char times
    }
    // Add a small safety margin to the silence window to tolerate
    // late-arriving CRC/echo bytes due to scheduling/jitter.
    // Increase by 1ms as requested for stability at lower baud rates.
    _silenceTimeUs += 3000;
    LOG_I("Silence time set to %lu us (including +3000us margin)", (unsigned long)_silenceTimeUs);
    
    // Initialize interval stats start time
    _intervalStats.intervalStartMs = millis();
    
    _rxBuffer.reserve(256);

    // Initialize frame history to avoid returning uninitialized garbage in /api/modbus/monitor
    for (size_t i = 0; i < FRAME_HISTORY_SIZE; i++) {
        _frameHistory[i].unitId = 0;
        _frameHistory[i].functionCode = 0;
        _frameHistory[i].dataLen = 0;
        _frameHistory[i].crc = 0;
        _frameHistory[i].timestamp = 0;
        _frameHistory[i].unixTimestamp = 0;
        _frameHistory[i].isRequest = false;
        _frameHistory[i].isValid = false;
        _frameHistory[i].isException = false;
        _frameHistory[i].exceptionCode = 0;
    }

    for (size_t i = 0; i < CRC_CONTEXT_SIZE; i++) {
        _crcContexts[i] = CrcErrorContext{};
        _crcContexts[i].before.timestamp = 0;
        _crcContexts[i].bad.timestamp = 0;
        _crcContexts[i].after.timestamp = 0;
    }
}

void ModbusRTUFeature::setResponseTimeoutMs(uint32_t ms) {
    _responseTimeoutMs = ms;
    LOG_I("Modbus response timeout set to %lu ms", (unsigned long)_responseTimeoutMs);
}

ModbusRTUFeature::LoopTiming ModbusRTUFeature::getLastLoopTiming() const {
    return _lastLoopTiming;
}

void ModbusRTUFeature::recordDebugSample(const char* name, uint32_t durUs) {
    if (_dbgSampleCount < DBG_MAX_SAMPLES) {
        _dbgSamples[_dbgSampleCount].name = name;
        _dbgSamples[_dbgSampleCount].durUs = durUs;
        ++_dbgSampleCount;
    } else {
        // overwrite oldest (simple circular behavior)
        int idx = _dbgSampleCount % DBG_MAX_SAMPLES;
        _dbgSamples[idx].name = name;
        _dbgSamples[idx].durUs = durUs;
        ++_dbgSampleCount;
    }
}

const ModbusRTUFeature::CrcErrorContext* ModbusRTUFeature::getRecentCrcErrorContexts(size_t& outCount) const {
    outCount = CRC_CONTEXT_SIZE;
    return _crcContexts;
}

void ModbusRTUFeature::recordFrameToHistory(const ModbusFrame& frame) {
    // Close pending CRC context with the "after" frame.
    if (_crcContextPendingNext) {
        CrcErrorContext& ctx = _crcContexts[_crcContextPendingIndex];
        if (!ctx.hasAfter) {
            ctx.after = frame;
            ctx.hasAfter = true;
        }
        _crcContextPendingNext = false;
    }

    _frameHistory[_frameHistoryIndex] = frame;
    _frameHistoryIndex = (_frameHistoryIndex + 1) % FRAME_HISTORY_SIZE;

    if (!frame.isValid) {
        recordCrcErrorContext(frame);
    }
}

void ModbusRTUFeature::recordCrcErrorContext(const ModbusFrame& badFrame) {
    CrcErrorContext& ctx = _crcContexts[_crcContextIndex];
    ctx = CrcErrorContext{};
    ctx.id = _crcContextNextId++;
    ctx.bad = badFrame;

    // Before = most recent previously recorded frame (best-effort)
    const size_t prevIdx = (_frameHistoryIndex + FRAME_HISTORY_SIZE - 2) % FRAME_HISTORY_SIZE;
    const ModbusFrame& prev = _frameHistory[prevIdx];
    if (prev.timestamp != 0) {
        ctx.before = prev;
        ctx.hasBefore = true;
    }

    // After will be filled by the next recorded frame
    ctx.hasAfter = false;
    _crcContextPendingNext = true;
    _crcContextPendingIndex = _crcContextIndex;

    _crcContextIndex = (_crcContextIndex + 1) % CRC_CONTEXT_SIZE;
}

String ModbusRTUFeature::formatFrameHex(const ModbusFrame& frame) const {
    // Build a byte array: unit + fc + payload + crc(lo,hi)
    size_t total = 2 + frame.dataLen + 2;
    std::vector<uint8_t> buf;
    buf.reserve(total);
    buf.push_back(frame.unitId);
    buf.push_back(frame.functionCode);
    for (size_t i = 0; i < frame.dataLen; ++i) buf.push_back(frame.data[i]);
    buf.push_back((uint8_t)(frame.crc & 0xFF));
    buf.push_back((uint8_t)((frame.crc >> 8) & 0xFF));
    std::string s = format_hex(buf.data(), buf.size());
    return String(s.c_str());
}

uint16_t ModbusRTUFeature::calculateFrameCrc(const ModbusFrame& frame) const {
    // Reconstruct the bytes that CRC is computed over: unit + fc + payload.
    // For exception frames, the payload is the single exception code byte.
    uint8_t bytes[2 + ModbusFrame::MAX_DATA_LEN];
    size_t len = 0;
    bytes[len++] = frame.unitId;
    bytes[len++] = frame.functionCode;
    if (frame.isException) {
        bytes[len++] = frame.exceptionCode;
    } else {
        const size_t copyLen = (frame.dataLen <= ModbusFrame::MAX_DATA_LEN) ? (size_t)frame.dataLen : ModbusFrame::MAX_DATA_LEN;
        if (copyLen > 0) {
            memcpy(bytes + len, frame.data.data(), copyLen);
            len += copyLen;
        }
    }
    return calculateCRC(bytes, len);
}

void ModbusRTUFeature::setup() {
    if (_ready) return;
    
    // Initialize DE pin for RS485
    if (_dePin >= 0) {
        pinMode(_dePin, OUTPUT);
        setDE(false);  // Start in receive mode
    }
    
    // Initialize serial
    if (_rxPin >= 0 && _txPin >= 0) {
        _serial.begin(_baudRate, _config, _rxPin, _txPin);
    } else {
        _serial.begin(_baudRate, _config);
    }
    
    _lastActivityTime = millis();
    _lastByteTime = micros();
    _serialWasEmpty = (_serial.available() == 0);
    _serialEmptySinceUs = micros();
    _lastWarningCheckMs = millis();
    _stats.lastStatsReset = millis();
    
    LOG_I("ModbusRTU initialized: %lu baud, silence=%lu us", _baudRate, _silenceTimeUs);
    if (_dePin >= 0) {
        LOG_I("  RS485 DE pin: %d", _dePin);
    }
    
    _busByteStats.reset();
    // Wire BusPatternTracker transitions directly to GapPredictor
    _patternTracker.setTransitionCallback([this](uint64_t pred, uint64_t succ, uint32_t gapMs) {
        _gapPredictor.recordTransition(pred, succ, gapMs);
    });
    _gapPredictor.stats().startMs = millis();
    _ready = true;
}

/**
 * @brief Main periodic processing for Modbus RTU feature.
 *
 * Handles RX buffering, frame extraction, transmission arbitration and
 * response timeouts. This function should be non-blocking and is invoked
 * frequently from the main `loop()`.
 */

void ModbusRTUFeature::loop() {
    if (!_ready) return;
    
    // When suspended, skip all processing (OTA in progress)
    if (_suspended) return;

    _loopCounter++;
    unsigned long loopStartUs = micros();

    // Start a new debug loop id and clear per-loop samples
    _dbgLoopId++;
    _dbgSampleCount = 0;

    // reset last-loop timing counters for this iteration
    _lastLoopTiming.rxReadUs = 0;
    _lastLoopTiming.rxProcessUs = 0;
    _lastLoopTiming.queueProcessUs = 0;
    _lastLoopTiming.loopUs = 0;
    
    unsigned long nowUs = loopStartUs;
    unsigned long nowMs = millis();
    
    // Track total time for statistics
    _stats.totalTimeUs += (nowUs - _activeStartTimeUs);
    if (!_inActiveTime) {
        _activeStartTimeUs = nowUs;
    }
    
    // Read available data from UART FIFO into the RX buffer.
    // IMPORTANT: Do NOT use per-byte micros() timestamps to detect inter-character
    // gaps (1.5 char times). The UART hardware buffers bytes and we read them in
    // bursts when loop() runs.  The read-time gap between the last byte of the
    // previous loop() call and the first byte of this one includes loop processing
    // time—NOT actual bus silence.  At 9600 baud the 1.5 char threshold is ~1.56ms,
    // but a typical ESP32 loop() period is 2-5ms, so every loop with bytes would
    // falsely trigger processReceivedData(), tearing valid frames into 2-3 byte
    // chunks.  Instead we rely SOLELY on the 3.5 char silence check below, plus
    // the scanning parser in processReceivedData() to handle concatenated frames.
    const bool wantsToTransmitSoon = (!_waitingForResponse && !_requestQueue.empty());
    const size_t maxRxBytesThisLoop = wantsToTransmitSoon ? 1024 : 256;
    unsigned long rxReadStartUs = micros();
    size_t rxBytesThisLoop = 0;
    while (_serial.available() && rxBytesThisLoop < maxRxBytesThisLoop) {
        unsigned long byteTimeUs = micros();
        uint8_t byte = _serial.read();
        rxBytesThisLoop++;

        if (_rxBuffer.empty()) {
            _rxBufferStartUs = (uint32_t)byteTimeUs;
            _rxBufferStartMs = (uint32_t)millis();
        }

        _rxBuffer.push_back(byte);
        _lastByteTime = byteTimeUs;
        _lastActivityTime = millis();
        _busSilent = false;

        // We observed RX data; the serial buffer is not empty.
        _serialWasEmpty = false;

        // Start tracking active time if not already
        if (!_inActiveTime && !_waitingForResponse) {
            startActiveTime(false);  // Assume other device traffic
        }
    }

    // record RX read duration
    _lastLoopTiming.rxReadUs = (uint32_t)(micros() - rxReadStartUs);
    CpuMonitor::recordFeatureDuration("ModbusRX", _lastLoopTiming.rxReadUs);
    recordDebugSample("ModbusRX", _lastLoopTiming.rxReadUs);

    _dbgRxBytesDrainedInLoop = (uint16_t)rxBytesThisLoop;

    // Re-evaluate current time before checking for frame-complete silence
    nowUs = micros();

    // Track when the UART RX buffer is observed empty. This is more reliable for deciding
    // when it's safe to transmit than using _lastByteTime alone, because bytes can sit
    // buffered until we get CPU time (then get timestamped "late" at read time).
    if (_serial.available() == 0) {
        if (!_serialWasEmpty) {
            _serialWasEmpty = true;
            _serialEmptySinceUs = nowUs;
        }
    } else {
        _serialWasEmpty = false;
    }

    // Check for frame complete (3.5 char silence)
    if (_rxBuffer.size() > 0 && (nowUs - _lastByteTime) > _silenceTimeUs) {
        unsigned long rxProcStartUs = micros();
        processReceivedData();
        _lastLoopTiming.rxProcessUs = (uint32_t)(micros() - rxProcStartUs);
        if (_lastLoopTiming.rxProcessUs > 0) {
            CpuMonitor::recordFeatureDuration("ModbusParse", _lastLoopTiming.rxProcessUs);
            recordDebugSample("ModbusParse", _lastLoopTiming.rxProcessUs);
        }
    } else {
        _lastLoopTiming.rxProcessUs = 0;
    }
    
    // Update bus silence state and end active time tracking
    // IMPORTANT: Use microsecond timing for the RTU silence window.
    // Using millis() rounding can prevent ever reaching the 3.5 char silence threshold
    // (e.g. 9600 baud => ~3.64ms required, but millis() jumps in 1ms steps).
    if (!_busSilent && (nowUs - _lastByteTime) > _silenceTimeUs) {
        _busSilent = true;
        if (_inActiveTime && !_waitingForResponse) {
            endActiveTime();
        }
    }
    
    // Response timeout handling moved to helper
    handleResponseTimeouts(nowUs, nowMs);
    
    // Process request queue when not waiting and there is a detectable inter-frame gap.
    // We attempt two strategies:
    // 1) Fast-path: If we observed the UART RX buffer empty long enough.
    // 2) Bounded arbitration: When requests are queued but our main loop is slow, we can
    //    miss the exact moment the RX buffer becomes empty. In that case, spend a very
    //    small, bounded time window actively watching for a quiet line and then transmit.
    if (!_waitingForResponse) {
        _dbgQueueSizeInLoop = (uint16_t)_requestQueue.size();
        _dbgWaitingForResponseInLoop = _waitingForResponse;
        _dbgSerialAvailableInLoop = (uint16_t)_serial.available();

        const uint32_t idleUs = _serialWasEmpty ? (uint32_t)(nowUs - _serialEmptySinceUs) : 0;
        const uint32_t requiredIdleUs = _silenceTimeUs;  // 3.5 char times (Modbus RTU spec)
        bool gapEnoughForTx = _serialWasEmpty && (idleUs > requiredIdleUs);
        
        // Multi-master arbitration: if we saw a foreign request, wait for its response
        // before transmitting. This reduces collisions on busy multi-master buses.
        if (gapEnoughForTx && _sawForeignRequest) {
            // Check if we've waited long enough (foreign response timeout)
            if ((nowMs - _foreignRequestTimeMs) < FOREIGN_RESPONSE_TIMEOUT_MS) {
                gapEnoughForTx = false;  // Keep waiting
            } else {
                // Timeout - clear flag and proceed
                _sawForeignRequest = false;
            }
        }

        _dbgGapUsInLoop = idleUs;
        _dbgGapEnoughForTxInLoop = gapEnoughForTx;
        _dbgLastLoopSnapshotMs = millis();

        if (gapEnoughForTx) {
            unsigned long qStartUs = micros();
            processQueue(true);  // Bus is silent, allow probing backoff units
            _lastLoopTiming.queueProcessUs = (uint32_t)(micros() - qStartUs);
            if (_lastLoopTiming.queueProcessUs > 0) {
                CpuMonitor::recordFeatureDuration("ModbusQueue", _lastLoopTiming.queueProcessUs);
                recordDebugSample("ModbusQueue", _lastLoopTiming.queueProcessUs);
            }
        } else if (!_requestQueue.empty()) {
            // Try to find a quiet window, bounded to keep the firmware responsive.
            static constexpr uint32_t TX_ARBITRATION_WINDOW_US = 8000;
            uint32_t startUs = micros();
            uint32_t lastRxUs = micros();

            // Prime lastRxUs with the most recent byte timestamp we have.
            if (!_serialWasEmpty) {
                lastRxUs = (uint32_t)_lastByteTime;
            }

            while ((uint32_t)(micros() - startUs) < TX_ARBITRATION_WINDOW_US) {
                if (_serial.available()) {
                    // Drain a bit more and keep our timestamps fresh.
                    uint8_t byte = _serial.read();
                    unsigned long byteTimeUs = micros();
                    lastRxUs = (uint32_t)byteTimeUs;
                    _serialWasEmpty = false;

                    if (_rxBuffer.empty()) {
                        _rxBufferStartUs = (uint32_t)byteTimeUs;
                        _rxBufferStartMs = (uint32_t)millis();
                    }
                    _rxBuffer.push_back(byte);
                    _lastByteTime = byteTimeUs;
                    _lastActivityTime = millis();
                    _busSilent = false;
                    continue;
                }

                // No buffered bytes right now. If we've been quiet long enough, transmit.
                uint32_t nowArbUs = micros();
                if ((uint32_t)(nowArbUs - lastRxUs) >= requiredIdleUs) {
                    _serialWasEmpty = true;
                    _serialEmptySinceUs = nowArbUs;
                    unsigned long qStartUs = micros();
                    processQueue(true);  // Bus is silent, allow probing backoff units
                    _lastLoopTiming.queueProcessUs = (uint32_t)(micros() - qStartUs);
                    if (_lastLoopTiming.queueProcessUs > 0) {
                        CpuMonitor::recordFeatureDuration("ModbusQueue", _lastLoopTiming.queueProcessUs);
                        recordDebugSample("ModbusQueue", _lastLoopTiming.queueProcessUs);
                    }
                    break;
                }

                // Short yield to avoid a tight spin.
                delayMicroseconds(50);
            }
        }
    }
    
    // Periodic warning check
    if (nowMs - _lastWarningCheckMs >= MODBUS_STATS_INTERVAL_MS) {
        checkAndLogWarnings();
        _lastWarningCheckMs = nowMs;
    }

    // Record total loop time and update historical max
    {
        unsigned long loopEndUs = micros();
        uint32_t loopUs = (uint32_t)(loopEndUs - loopStartUs);
        _lastLoopTiming.loopUs = loopUs;
        if (loopUs > _lastLoopTiming.maxLoopUs) _lastLoopTiming.maxLoopUs = loopUs;
    }

    // Process a few deferred parsed frames to spread heavy work across loops
    // Keep budget small to avoid reintroducing large spikes.
    (void)processParsedFrames(4, 2000);

    // Correlation check: ensure no single recorded subfeature exceeds this loop's total
    // Add small slack for measurement jitter.
    {
        const uint32_t slackUs = 50;
        uint32_t loopUs = _lastLoopTiming.loopUs;
        int count = (_dbgSampleCount < DBG_MAX_SAMPLES) ? _dbgSampleCount : DBG_MAX_SAMPLES;
        for (int si = 0; si < count; ++si) {
            const LoopSample& s = _dbgSamples[si];
            if (s.durUs > loopUs + slackUs) {
                LOG_W("Modbus loop-correlation anomaly: loopId=%u loopUs=%u sub=%s dur=%u (slack=%u)",
                      (unsigned)_dbgLoopId, (unsigned)loopUs, s.name ? s.name : "<null>", (unsigned)s.durUs, (unsigned)slackUs);
                // Also dump all samples for this loop at verbose level for further inspection
                for (int sj = 0; sj < count; ++sj) {
                    const LoopSample& ss = _dbgSamples[sj];
                    LOG_D("  sample[%d]=%s %uus", sj, ss.name ? ss.name : "<null>", (unsigned)ss.durUs);
                }
                break; // one report is enough per loop
            }
        }
    }
}


/**
 * @brief Handle buffered RX bytes and extract complete Modbus frames.
 *
 * This function is invoked when a 3.5-char silence is detected; it
 * delegates to parsing helpers and then clears the RX buffer.
 */
void ModbusRTUFeature::processReceivedData() {
    if (_rxBuffer.size() < 4) {
        if (_rxBuffer.size() > 0) onFrameBoundary(_rxBuffer.size());
        return;
    }

    onFrameBoundary(_rxBuffer.size());

    // Delegate to extracted helpers. Only remove bytes that were consumed
    // (complete frames). Keep any partial trailing bytes in the buffer so
    // they can be completed on the next loop without blocking here.
    size_t consumed = extractFramesFromRxBuffer();
    if (consumed > 0) {
        if (consumed < _rxBuffer.size()) {
            // erase consumed prefix
            _rxBuffer.erase(_rxBuffer.begin(), _rxBuffer.begin() + (ptrdiff_t)consumed);
        } else {
            _rxBuffer.clear();
        }
    }

    // Safety: prevent unbounded RX buffer growth. If buffer gets very large
    // (likely due to noise), trim it and record an error to avoid memory issues.
    static const size_t RX_BUFFER_MAX = 2048;
    if (_rxBuffer.size() > RX_BUFFER_MAX) {
        _stats.crcErrors++;
        // keep only the last RX_BUFFER_MAX/2 bytes
        size_t keep = RX_BUFFER_MAX / 2;
        _rxBuffer.erase(_rxBuffer.begin(), _rxBuffer.end() - (ptrdiff_t)keep);
    }
}

// Extract frames from _rxBuffer and handle them. Returns number of bytes consumed.
size_t ModbusRTUFeature::extractFramesFromRxBuffer() {
    // Spec-based extraction constants (focus: FC3/FC4)
    static constexpr uint8_t FC3 = ModbusFC::READ_HOLDING_REGISTERS;
    static constexpr uint8_t FC4 = ModbusFC::READ_INPUT_REGISTERS;
    static constexpr uint8_t FC3_EX = (uint8_t)(FC3 | 0x80);
    static constexpr uint8_t FC4_EX = (uint8_t)(FC4 | 0x80);
    static constexpr uint8_t MAX_RTU_UNIT_ID = 247;
    static constexpr uint16_t MAX_REGS_PER_READ = 125;
    static constexpr uint8_t MAX_BYTECOUNT = 250;

    size_t i = 0;
    size_t extractedCount = 0;
    bool sawNoise = false;

    // Use scan-and-dispatch helper to keep main loop concise
    unsigned long scanStartUs = micros();
    size_t consumed = scanAndAdvanceIndex();
    _lastLoopTiming.scanUs = (uint32_t)(micros() - scanStartUs);
    if (_lastLoopTiming.scanUs > 0) {
        CpuMonitor::recordFeatureDuration("ModbusScan", _lastLoopTiming.scanUs);
        recordDebugSample("ModbusScan", _lastLoopTiming.scanUs);
    }
    if (_lastLoopTiming.scanWaitUs > 0) {
        CpuMonitor::recordFeatureDuration("ModbusWait", _lastLoopTiming.scanWaitUs);
        recordDebugSample("ModbusWait", _lastLoopTiming.scanWaitUs);
    }
    // per-frame parse/update durations are recorded individually in
    // parseFrameAndComputeMetadata() and updateRegisterMap()

    if ((i < _rxBuffer.size()) || (sawNoise && extractedCount == 0)) {
        _stats.crcErrors++;
    }

    (void)extractedCount;
    return consumed;
}

/**
 * @brief Attempt to parse and handle frames from the RX buffer.
 *
 * Uses `scanAndAdvanceIndex()` to find frames, validate CRC and dispatch
 * responses/requests to appropriate handlers.
 * @return Number of bytes left in RX buffer after processing.
 */

// Small parser helpers
bool ModbusRTUFeature::tryParseAtLen(const uint8_t* p, size_t remaining, size_t len, ModbusFrame& out) {
    return ModbusRTUHelper::tryParseAtLen(p, remaining, len, out, (uint32_t)millis(), TimeUtils::nowUnixSecondsOrZero());
}

bool ModbusRTUFeature::determineFrameLength(const uint8_t* p, size_t remaining, bool& isRequest, size_t& frameLen) {
    return ModbusRTUHelper::determineFrameLength(p, remaining, isRequest, frameLen);
}

void ModbusRTUFeature::parseFrameAndComputeMetadata(size_t offset, size_t frameLen, ModbusFrame& frame, bool isRequest, uint32_t approxStartMs) {
    // parseFrame expects pointer to start of frame
    unsigned long pStart = micros();
    parseFrame(_rxBuffer.data() + offset, frameLen, frame);
    uint32_t pUs = (uint32_t)(micros() - pStart);
    _lastLoopTiming.parseUnitUs += pUs;
    // record per-frame parse duration so CpuMonitor tracks min/avg/max per-sample
    CpuMonitor::recordFeatureDuration("ModbusParseUnit", pUs);
    recordDebugSample("ModbusParseUnit", pUs);
    frame.timestamp = approxStartMs;
    frame.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
    frame.isRequest = isRequest;
}

void ModbusRTUFeature::handleCrcInvalidFrame(const ModbusFrame& frame, bool resyncAllowed) {
    if (!resyncAllowed) return;
    if (!_inResync) {
        _stats.crcErrors++;
        _busByteStats.invalidFrames++;
        LOG_W("RX Frame (CRC ERROR): Unit=%d, FC=0x%02X, Raw=%s",
              frame.unitId, frame.functionCode, formatFrameHex(frame).c_str());
        _gapWindowActive = false;
        _ownTxInCurrentGap = 0;
    } else {
        LOG_V("RX resync attempt: Unit=%d, FC=0x%02X (not counted)", frame.unitId, frame.functionCode);
    }
    // Note: recordFrameToHistory and _frameCallback are called by scanAndAdvanceIndex
    _inResync = true;
}

/**
 * @brief Handle a frame that was parsed from the RX buffer.
 *
 * This performs matching against pending requests, or forwards foreign
 * frames to the foreign request/response handlers.
 */
void ModbusRTUFeature::handleOurResponse(const ModbusFrame& frame, size_t frameLen) {
    // matched our in-flight request
    _waitingForResponse = false;
    // Keep stats disabled through our entire TX-response window; will re-enable on next foreign request
    uint32_t rtt = (uint32_t)(millis() - _requestSentTime);
    _busTransactionStats.record(rtt);
    _backoffByUnit.erase(frame.unitId);
    _lastSuccessTime = millis();

    if (!frame.isException) {
        _stats.ownRequestsSuccess++;
        _intervalStats.ownSuccess++;
        _gapPredictor.stats().registersRead += _currentRequest.quantity;
        if (_sentDuringGapWindow) {
                    _gapPredictor.noteGapSuccess();
        }
        _sentDuringGapWindow = false;
    } else {
        _stats.ownRequestsFailed++;
        _intervalStats.ownFailed++;
        _sentDuringGapWindow = false;
        LOG_W("Modbus exception 0x%02X from unit %d", frame.exceptionCode, frame.unitId);
    }

    updateRegisterMap(_lastRequest, frame);

    std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
    if (_currentRequest.callback) callbackCopy = _currentRequest.callback;
    _hasPendingRequest = false;
    if (callbackCopy) {
        try { callbackCopy(!frame.isException, frame); } catch (...) { LOG_E("Exception in Modbus response callback"); }
    }
    endActiveTime();
    _lastTransactionEndMs = millis();
    _hasLastTransactionEnd = true;
}

void ModbusRTUFeature::handleForeignRequest(const ModbusFrame& frame) {
    // possible TX echo check
    if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
        frame.unitId == _currentRequest.unitId &&
        ((frame.functionCode & 0x7F) == (_currentRequest.functionCode & 0x7F)) &&
        (frame.getStartRegister() == _currentRequest.startRegister) &&
        (frame.getQuantity() == _currentRequest.quantity)) {
        // echo - ignore
        return;
    }

    uint8_t reqFc = frame.functionCode & 0x7F;
    if (isReadFunction(reqFc)) {
        ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, reqFc);
        map.requestCount++;
        map.lastUpdate = millis();
        
        // Track distinct quantities seen from foreign masters
        uint16_t qty = frame.getQuantity();
        if (qty > 0) {
            _foreignRequestQuantities.insert(qty);
        }
    }

    // Re-enable stats gathering now that we've seen the next foreign request cleanly
    _gapPredictor.setStatsEnabled(true);

    _lastRequestPerUnit[frame.unitId] = frame;
    _stats.otherRequestsSeen++;
    recordBusPattern(frame);
    _sawForeignRequest = true;
    _foreignRequestTimeMs = millis();
    _gapWindowActive = false;
    _ownTxInCurrentGap = 0;
    startActiveTime(false);
}

void ModbusRTUFeature::handleForeignResponse(const ModbusFrame& frame, size_t frameLen) {
    if (frame.isException) {
        _stats.otherExceptionsSeen++;
        _intervalStats.otherFailed++;
        _sawForeignRequest = false;

        uint8_t exFc = frame.functionCode & 0x7F;
        bool paired = false;
        if (isReadFunction(exFc)) {
            auto reqIt = _lastRequestPerUnit.find(frame.unitId);
            if (reqIt != _lastRequestPerUnit.end()) {
                const ModbusFrame& req = reqIt->second;
                if (req.isValid && ((req.functionCode & 0x7F) == exFc) && req.dataLen == 4) {
                    if ((frame.timestamp - req.timestamp) < 2000) paired = true;
                }
            }
            if (paired) _stats.otherExceptionsPaired++; else _stats.otherExceptionsUnpaired++;
        }

        if (isReadFunction(exFc)) {
            ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, exFc);
            map.responseCount++;
            map.errorCount++;
            map.lastUpdate = millis();
        }
        return;
    }

    _stats.otherResponsesSeen++;
    _intervalStats.otherSuccess++;
    _sawForeignRequest = false;

    auto reqIt = _lastRequestPerUnit.find(frame.unitId);
    bool updated = false;
    if (reqIt != _lastRequestPerUnit.end()) {
        const ModbusFrame& req = reqIt->second;
        if (req.isValid && ((req.functionCode & 0x7F) == (frame.functionCode & 0x7F)) && req.dataLen == 4) {
            if ((frame.timestamp - req.timestamp) < 2000) {
                updateRegisterMap(req, frame);
                updated = true;
                uint32_t respEndMs = frame.timestamp + (uint32_t)((uint64_t)frameLen * _charTimeUs / 1000ULL);
                uint32_t rtt = (uint32_t)(respEndMs - req.timestamp);
                _busTransactionStats.record(rtt);

                uint16_t startReg = (req.data[0] << 8) | req.data[1];
                uint16_t qty      = (req.data[2] << 8) | req.data[3];
                _lastCompletedTxKey = encodePatternKey(req.unitId, req.functionCode, startReg, qty);
                _hasLastCompletedTx = true;
                _lastTransactionEndMs = respEndMs;
                _hasLastTransactionEnd = true;
            }
        }
    }

    uint8_t respFc = frame.functionCode & 0x7F;
    if (!updated && isReadFunction(respFc)) {
        ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, respFc);
        map.responseCount++;
        map.lastUpdate = millis();
    }

    if (isReadFunction(respFc)) {
        if (updated) _stats.otherResponsesPaired++; else _stats.otherResponsesUnpaired++;
    }

    if (updated && _hasLastCompletedTx) {
        _gapWindowOpenMs = millis();
        _gapWindowActive = true;
        _gapWindowUsedMs = 0;
    }
}

void ModbusRTUFeature::finishFrameProcessingAndNotify(const ModbusFrame& frame, bool isRequest) {
    // record history and call frame callback
    recordFrameToHistory(frame);
    if (_frameCallback) _frameCallback(frame, isRequest);
}

size_t ModbusRTUFeature::scanAndAdvanceIndex() {
    size_t i = 0;
    size_t consumed = 0;
    bool sawNoise = false;
    size_t extractedCount = 0;

    while (i + MIN_FRAME_SIZE <= _rxBuffer.size()) {
        const uint8_t* p = _rxBuffer.data() + i;
        size_t remaining = _rxBuffer.size() - i;

        uint8_t unitId = p[0];
        if (unitId == 0 || unitId > MAX_RTU_UNIT_ID) { sawNoise = true; i++; continue; }

        bool isRequest = false;
        size_t frameLen = 0;
        if (!determineFrameLength(p, remaining, isRequest, frameLen)) {
            // Before skipping, check if we're waiting for a response and might
            // just be missing trailing bytes (e.g., byte 9/9 arriving late).
            // For FC3/FC4 responses, byteCount indicates exact expected length.
            bool waitedForTrailing = false;
            if (_waitingForResponse && _hasPendingRequest && remaining >= 3) {
                uint8_t fc = p[1];
                if (p[0] == _currentRequest.unitId && (fc == 0x03 || fc == 0x04)) {
                    uint8_t byteCount = p[2];
                    if (byteCount >= 2 && (byteCount % 2) == 0 && byteCount <= 250) {
                        size_t expectedLen = (size_t)byteCount + 5; // unit+fc+bc+payload+crc
                        if (remaining < expectedLen && remaining >= expectedLen - 2) {
                            // Missing 1-2 bytes at end of buffer. Do NOT block/wait here;
                            // return consumed bytes so far and let the next loop
                            // iteration complete the frame when bytes arrive.
                            return i;
                        }
                    }
                }
            }
            sawNoise = true; i++; continue;
        }
        frame_determined:

        const uint32_t approxStartMs = _rxBufferStartMs + (uint32_t)((uint64_t)i * (uint64_t)_charTimeUs / 1000ULL);
        ModbusFrame frame;
        parseFrameAndComputeMetadata(i, frameLen, frame, isRequest, approxStartMs);

        // Record all frames to history immediately (before dispatch)
        // Callbacks are invoked once per frame after dispatch logic below
        recordFrameToHistory(frame);
        bool skipFrameCallback = false;

        if (!frame.isValid) {
            // Best-effort: if we are waiting for a response for this unit and
            // the invalid frame structurally matches our expected response
            // (function code and byte count), accept it as a fallback. This
            // helps in noisy/multi-master environments where collisions can
            // corrupt the CRC but leave the payload intact.
            // If the frame is invalid and it's for our pending request, allow
            // a very short window to collect trailing bytes that may have
            // arrived slightly after we observed silence (scheduling/jitter).
            // This avoids false CRC failures when the final CRC byte arrives
            // a few hundred microseconds late.
            if (_waitingForResponse && _hasPendingRequest && frame.unitId == _currentRequest.unitId) {
                // Do not block waiting for late CRC byte(s). Continue with
                // invalid handling immediately; any additional bytes will be
                // processed in a later loop when they arrive.
            }
            if (_waitingForResponse && _hasPendingRequest && !frame.isRequest && frame.unitId == _currentRequest.unitId) {
                // Debug: record the context so we can see why fallback may be rejected
                LOG_D("CRC-invalid frame for unit %u while waiting for unit %u (req start=%u qty=%u) isRequest=%d fc=0x%02X byteCount=%u", 
                      frame.unitId, _currentRequest.unitId, _currentRequest.startRegister, _currentRequest.quantity, (int)frame.isRequest, frame.functionCode, (unsigned)frame.getByteCount());
            }

            handleCrcInvalidFrame(frame, true);
            i++; // advance one to resync
            continue;
        }

        // valid frame: update counters
        _inResync = false;
        _stats.framesReceived++;
        _busByteStats.validFrames++;

        // dispatch
        if (_waitingForResponse && _hasPendingRequest && !frame.isRequest && frame.unitId == _currentRequest.unitId) {
            // check matching
            const uint8_t expectedFc = _currentRequest.functionCode;
            const uint8_t expectedFcBase = (uint8_t)(expectedFc & 0x7F);
            const bool fcMatches = (frame.functionCode == expectedFc) || (frame.isException && ((frame.functionCode & 0x7F) == expectedFcBase));
            bool byteCountMatches = true;
            if (!frame.isException && isReadFunction(expectedFcBase)) {
                byteCountMatches = (frame.getByteCount() == (size_t)_currentRequest.quantity * 2);
            }
            if (frame.isValid && fcMatches && byteCountMatches) {
                // Enforce strict timing window before accepting response
                uint32_t elapsedMs = (uint32_t)(millis() - _requestSentTime);
                if (elapsedMs < RESPONSE_MIN_WINDOW_MS || elapsedMs > RESPONSE_MAX_WINDOW_MS) {
                    // Outside allowed window - treat as mismatch and log details
                    LOG_W("RX response outside strict window: unit=%d elapsed=%ums (min=%u max=%u) req=unit:%u fc:0x%02X reg:%u qty:%u resp=%s",
                          frame.unitId, elapsedMs, RESPONSE_MIN_WINDOW_MS, RESPONSE_MAX_WINDOW_MS,
                          _currentRequest.unitId, _currentRequest.functionCode,
                          _currentRequest.startRegister, _currentRequest.quantity,
                          formatFrameHex(frame).c_str());
                    // Record mismatch
                    ResponseMismatch& m = _mismatchHistory[_mismatchIndex];
                    m.timestamp = millis();
                    m.expectedUnit = _currentRequest.unitId;
                    m.actualUnit = frame.unitId;
                    m.expectedFc = _currentRequest.functionCode;
                    m.actualFc = frame.functionCode;
                    m.byteCountMatch = byteCountMatches;
                    _mismatchIndex = (_mismatchIndex + 1) % MISMATCH_HISTORY_SIZE;
                    _mismatchCount++;
                    // These frames likely contain garbage — do not notify global listeners
                    skipFrameCallback = true;
                } else {
                    handleOurResponse(frame, frameLen);
                }
            } else {
                // mismatch
                ResponseMismatch& m = _mismatchHistory[_mismatchIndex];
                m.timestamp = millis();
                m.expectedUnit = _currentRequest.unitId;
                m.actualUnit = frame.unitId;
                m.expectedFc = _currentRequest.functionCode;
                m.actualFc = frame.functionCode;
                m.byteCountMatch = byteCountMatches;
                _mismatchIndex = (_mismatchIndex + 1) % MISMATCH_HISTORY_SIZE;
                _mismatchCount++;
                LOG_W("RX mismatch: unit=%d/%d fc=%d/%d byteCount=%s", frame.unitId, _currentRequest.unitId, frame.functionCode, _currentRequest.functionCode, byteCountMatches ? "ok" : "MISMATCH");
            }
            // Notify listeners unless we've explicitly suppressed callbacks for this frame
            if (!skipFrameCallback && _frameCallback) _frameCallback(frame, isRequest);
            i += frameLen; extractedCount++; continue;
        }

        if (frame.isRequest) {
            handleForeignRequest(frame);
            if (_frameCallback) _frameCallback(frame, isRequest);
            i += frameLen; extractedCount++; continue;
        }

        // response from other device
        handleForeignResponse(frame, frameLen);
        if (_frameCallback) _frameCallback(frame, isRequest);
        i += frameLen; extractedCount++; continue;
    }

    (void)sawNoise; (void)extractedCount;
    return i;
}

void ModbusRTUFeature::handleResponseTimeouts(unsigned long nowUs, unsigned long nowMs) {
    if (!(_waitingForResponse && (nowMs - _requestSentTime) > _responseTimeoutMs)) return;

    _stats.timeouts++;

    uint16_t unitKey = _lastRequest.unitId;
    unsigned long lastLog = _lastTimeoutPerUnit[unitKey];
    if ((nowMs - lastLog) >= 5000) {  // 5 second throttle per unit
        LOG_W("Modbus response timeout for unit %d FC 0x%02X reg %d qty %d (retries: %d)",
              _lastRequest.unitId, _lastRequest.functionCode,
              _lastRequest.getStartRegister(), _lastRequest.getQuantity(),
              _currentRequest.retries);
        _lastTimeoutPerUnit[unitKey] = nowMs;
    }

    LOG_D("Timeout diagnostics: nowMs=%lu requestSentAt=%lu elapsedMs=%lu", nowMs, _requestSentTime, (nowMs - _requestSentTime));
    LOG_D("  Args: serialWasEmpty=%d serialEmptySinceUs=%lu lastByteTimeUs=%lu lastActivityMs=%lu", (int)_serialWasEmpty, (unsigned long)_serialEmptySinceUs, (unsigned long)_lastByteTime, (unsigned long)_lastActivityTime);
    LOG_D("  Loop dbg: rxDrained=%u gapUs=%u queueSize=%u serialAvail=%u", (unsigned)_dbgRxBytesDrainedInLoop, (unsigned)_dbgGapUsInLoop, (unsigned)_dbgQueueSizeInLoop, (unsigned)_dbgSerialAvailableInLoop);
    LOG_D("  Gap/scheduler: sentDuringGap=%d lastTxWireMs=%u lastTxElapsedMs=%u hasLastCompletedTx=%d lastCompletedTxKey=%llu", (int)_sentDuringGapWindow, (unsigned)_lastTxWireMs, (unsigned)_lastTxElapsedMs, (int)_hasLastCompletedTx, (unsigned long long)_lastCompletedTxKey);
    LOG_D("  Request params: unit=%u fc=0x%02X start=%u qty=%u retries=%u", (unsigned)_currentRequest.unitId, (unsigned)_currentRequest.functionCode, (unsigned)_currentRequest.startRegister, (unsigned)_currentRequest.quantity, (unsigned)_currentRequest.retries);

    if (_sentDuringGapWindow) {
        _gapPredictor.reportCollision(_sentDuringGapWindow, _lastTxElapsedMs, _lastTxWireMs,
                                      _hasLastCompletedTx, _lastCompletedTxKey);
        _sentDuringGapWindow = false;
    }

    if (_currentRequest.retries == 0) {
        LOG_I("Modbus timeout: re-queuing request (unit %d FC 0x%02X retry 1/1)",
              _currentRequest.unitId, _currentRequest.functionCode);
        _currentRequest.retries = 1;
        _requestQueue.insert(_requestQueue.begin(), _currentRequest);
        _waitingForResponse = false;
        _hasPendingRequest = false;
        endActiveTime();
        return;
    }

    _stats.ownRequestsFailed++;
    _intervalStats.ownFailed++;

    const uint8_t unitId = _currentRequest.unitId;
    TimeoutBackoffState& st = _backoffByUnit[unitId];
    st.consecutiveTimeouts++;
    if (st.consecutiveTimeouts >= 3) {
        st.pausedUntilMs = (uint32_t)nowMs + st.backoffMs;
        if (st.consecutiveTimeouts == 3) {
            LOG_W("Modbus: 3 consecutive timeouts for unit %u, pausing sends for %ums", unitId, st.backoffMs);
        }
        if (st.backoffMs < 60000) {
            st.backoffMs = std::min<uint32_t>(st.backoffMs * 2, 60000);
        }
    }

    std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
    if (_currentRequest.callback) callbackCopy = _currentRequest.callback;

    _waitingForResponse = false;
    _hasPendingRequest = false;
    endActiveTime();

    if (callbackCopy) {
        ModbusFrame emptyFrame;
        emptyFrame.isValid = false;
        emptyFrame.isException = false;
        try { callbackCopy(false, emptyFrame); } catch (...) { LOG_E("Exception in Modbus timeout callback"); }
    }

    if (_requestQueue.size() > _maxQueueSize / 2) {
        const size_t before = _requestQueue.size();
        _requestQueue.erase(
            std::remove_if(_requestQueue.begin(), _requestQueue.end(),
                           [unitId](const ModbusPendingRequest& r) { return r.unitId == unitId; }),
            _requestQueue.end());
        const size_t after = _requestQueue.size();
        if (after != before) {
            LOG_W("Modbus queue building up (%u items). Dropped %u requests for unit %u",
                  before, (unsigned)(before - after), unitId);
        }
    }
}

void ModbusRTUFeature::handleParsedFrame(const ModbusFrame& frame, bool isRequest, size_t frameLen) {
    bool isOurResponse = false;

    const uint8_t expectedFc = _currentRequest.functionCode;
    const uint8_t expectedFcBase = (uint8_t)(expectedFc & 0x7F);
    const bool fcMatches = (frame.functionCode == expectedFc) ||
                           (frame.isException && ((frame.functionCode & 0x7F) == expectedFcBase));

    bool byteCountMatches = true;
    static constexpr uint8_t FC3 = ModbusFC::READ_HOLDING_REGISTERS;
    static constexpr uint8_t FC4 = ModbusFC::READ_INPUT_REGISTERS;
    if (!frame.isException && (expectedFcBase == FC3 || expectedFcBase == FC4)) {
        const size_t expectedBytes = (size_t)_currentRequest.quantity * 2;
        const size_t actualBytes = frame.getByteCount();
        byteCountMatches = (actualBytes == expectedBytes);
    }

    if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
        !frame.isRequest && frame.unitId == _currentRequest.unitId &&
        fcMatches && byteCountMatches) {
        // Timing-window enforcement is performed in the scanner dispatch path;
        // if we've reached here the frame matches the pending request and
        // may be treated as our response.
        isOurResponse = true;
    }

    if (isOurResponse) {
        _waitingForResponse = false;

        uint32_t rtt = (uint32_t)(millis() - _requestSentTime);
        _busTransactionStats.record(rtt);

        _backoffByUnit.erase(frame.unitId);
        _lastSuccessTime = millis();

        if (!frame.isException) {
            _stats.ownRequestsSuccess++;
            _intervalStats.ownSuccess++;
            _gapPredictor.stats().registersRead += _currentRequest.quantity;
            if (_sentDuringGapWindow) {
                        _gapPredictor.noteGapSuccess();
            }
            _sentDuringGapWindow = false;
        } else {
            _stats.ownRequestsFailed++;
            _intervalStats.ownFailed++;
            _sentDuringGapWindow = false;
            LOG_W("Modbus exception 0x%02X from unit %d", frame.exceptionCode, frame.unitId);
        }

        updateRegisterMap(_lastRequest, frame);

        std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
        if (_currentRequest.callback) callbackCopy = _currentRequest.callback;
        _hasPendingRequest = false;

        if (callbackCopy) {
            try {
                callbackCopy(!frame.isException, frame);
            } catch (...) {
                LOG_E("Exception in Modbus response callback");
            }
        }

        endActiveTime();
        _lastTransactionEndMs = millis();
        _hasLastTransactionEnd = true;
        return;
    }

    if (_waitingForResponse && _hasPendingRequest && frame.isValid && !frame.isRequest) {
        ResponseMismatch& m = _mismatchHistory[_mismatchIndex];
        m.timestamp = millis();
        m.expectedUnit = _currentRequest.unitId;
        m.actualUnit = frame.unitId;
        m.expectedFc = _currentRequest.functionCode;
        m.actualFc = frame.functionCode;
        m.byteCountMatch = byteCountMatches;
        _mismatchIndex = (_mismatchIndex + 1) % MISMATCH_HISTORY_SIZE;
        _mismatchCount++;
        LOG_W("RX mismatch: unit=%d/%d fc=%d/%d byteCount=%s",
              frame.unitId, _currentRequest.unitId,
              frame.functionCode, _currentRequest.functionCode,
              byteCountMatches ? "ok" : "MISMATCH");
        return;
    }

    // Foreign traffic handling
    if (isRequest) {
        if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
            frame.unitId == _currentRequest.unitId &&
            ((frame.functionCode & 0x7F) == (_currentRequest.functionCode & 0x7F)) &&
            (frame.getStartRegister() == _currentRequest.startRegister) &&
            (frame.getQuantity() == _currentRequest.quantity)) {
            // Likely TX echo - ignore
            return;
        }

        uint8_t reqFc = frame.functionCode & 0x7F;
        if (reqFc == FC3 || reqFc == FC4) {
            ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, reqFc);
            map.requestCount++;
            map.lastUpdate = millis();
        }

        _lastRequestPerUnit[frame.unitId] = frame;
        _stats.otherRequestsSeen++;
        recordBusPattern(frame);
        _sawForeignRequest = true;
        _foreignRequestTimeMs = millis();
        _gapWindowActive = false;
        _ownTxInCurrentGap = 0;
        startActiveTime(false);
        return;
    }

    // Response (foreign)
    if (frame.isException) {
        _stats.otherExceptionsSeen++;
        _intervalStats.otherFailed++;
        _sawForeignRequest = false;

        uint8_t exFc = frame.functionCode & 0x7F;
        bool paired = false;
        if (exFc == FC3 || exFc == FC4) {
            auto reqIt = _lastRequestPerUnit.find(frame.unitId);
            if (reqIt != _lastRequestPerUnit.end()) {
                const ModbusFrame& req = reqIt->second;
                if (req.isValid && ((req.functionCode & 0x7F) == exFc) && req.dataLen == 4) {
                    if ((frame.timestamp - req.timestamp) < 2000) paired = true;
                }
            }
            if (paired) _stats.otherExceptionsPaired++; else _stats.otherExceptionsUnpaired++;
        }

        if (exFc == FC3 || exFc == FC4) {
            ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, exFc);
            map.responseCount++;
            map.errorCount++;
            map.lastUpdate = millis();
        }
        return;
    }

    // Non-exception response
    _stats.otherResponsesSeen++;
    _intervalStats.otherSuccess++;
    _sawForeignRequest = false;

    auto reqIt = _lastRequestPerUnit.find(frame.unitId);
    bool updated = false;
    if (reqIt != _lastRequestPerUnit.end()) {
        const ModbusFrame& req = reqIt->second;
        if (req.isValid && ((req.functionCode & 0x7F) == (frame.functionCode & 0x7F)) && req.dataLen == 4) {
            if ((frame.timestamp - req.timestamp) < 2000) {
                updateRegisterMap(req, frame);
                updated = true;
                uint32_t respEndMs = frame.timestamp + (uint32_t)((uint64_t)frameLen * _charTimeUs / 1000ULL);
                uint32_t rtt = (uint32_t)(respEndMs - req.timestamp);
                _busTransactionStats.record(rtt);

                uint16_t startReg = (req.data[0] << 8) | req.data[1];
                uint16_t qty      = (req.data[2] << 8) | req.data[3];
                _lastCompletedTxKey = encodePatternKey(req.unitId, req.functionCode, startReg, qty);
                _hasLastCompletedTx = true;
                _lastTransactionEndMs = respEndMs;
                _hasLastTransactionEnd = true;
            }
        }
    }

    uint8_t respFc = frame.functionCode & 0x7F;
    if (!updated && (respFc == FC3 || respFc == FC4)) {
        ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, respFc);
        map.responseCount++;
        map.lastUpdate = millis();
    }

    if (respFc == FC3 || respFc == FC4) {
        if (updated) _stats.otherResponsesPaired++; else _stats.otherResponsesUnpaired++;
    }

    if (updated && _hasLastCompletedTx) {
        _gapWindowOpenMs = millis();
        _gapWindowActive = true;
        _gapWindowUsedMs = 0;
    }
}

ModbusRegisterMap& ModbusRTUFeature::ensureRegisterMap(uint8_t unitId, uint8_t functionCode) {
    uint16_t key = makeMapKey(unitId, functionCode);

    // Create map if doesn't exist
    if (_registerMaps.find(key) == _registerMaps.end()) {
        ModbusRegisterMap newMap;
        newMap.unitId = unitId;
        newMap.functionCode = functionCode;
        newMap.lastUpdate = 0;
        newMap.requestCount = 0;
        newMap.responseCount = 0;
        newMap.errorCount = 0;
        _registerMaps[key] = newMap;
        LOG_I("Created register map for unit %u, FC 0x%02X", unitId, functionCode);
    }

    return _registerMaps[key];
}

bool ModbusRTUFeature::parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame) {
    return ModbusRTUHelper::parseModbusFrame(data, length, frame, (uint32_t)millis(), TimeUtils::nowUnixSecondsOrZero());
}

void ModbusRTUFeature::updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response) {
    // Defer heavy update work: enqueue a parsed-pair for later processing.
    scheduleRegisterUpdate(request, response, 0);
}

void ModbusRTUFeature::scheduleRegisterUpdate(const ModbusFrame& request, const ModbusFrame& response, size_t responseLen) {
    if (_parsedQueueCount >= PARSED_QUEUE_SIZE) {
        // queue full; drop and account for it
        _stats.crcErrors++; // reuse CRC error counter as a generic error metric
        LOG_W("Parsed queue full; dropping update for unit %u", response.unitId);
        return;
    }
    ParsedWork& w = _parsedQueue[_parsedQueueTail];
    w.request = request;
    w.response = response;
    w.hasRequest = true;
    w.hasResponse = true;
    w.responseLen = responseLen;
    _parsedQueueTail = (_parsedQueueTail + 1) % PARSED_QUEUE_SIZE;
    ++_parsedQueueCount;
}

size_t ModbusRTUFeature::processParsedFrames(size_t maxCount, uint32_t maxUs) {
    if (_parsedQueueCount == 0) return 0;
    unsigned long startUs = micros();
    size_t processed = 0;
    while (processed < maxCount && _parsedQueueCount > 0) {
        ParsedWork w = _parsedQueue[_parsedQueueHead];
        _parsedQueueHead = (_parsedQueueHead + 1) % PARSED_QUEUE_SIZE;
        --_parsedQueueCount;

        unsigned long uStart = micros();
        ModbusRegisterMap& regMap = ensureRegisterMap(w.response.unitId, w.response.functionCode);
        ModbusRTUHelper::updateModbusRegisterMap(regMap, w.request, w.response, (uint32_t)millis());
        uint32_t uUs = (uint32_t)(micros() - uStart);
        _lastLoopTiming.updateMapUs += uUs;
        CpuMonitor::recordFeatureDuration("ModbusUpdateMap", uUs);
        recordDebugSample("ModbusUpdateMap", uUs);

        // Also record bus pattern / other light-weight post-processing
        recordBusPattern(w.response);

        processed++;
        if ((uint32_t)(micros() - startUs) >= maxUs) break;
    }
    return processed;
}

/**
 * @brief Process the outgoing request queue: select a request and attempt to send it.
 *
 * This performs gap-aware scheduling and enforces per-unit backoff.
 */
void ModbusRTUFeature::processQueue(bool busSilent) {
    if (_requestQueue.empty()) return;

    // ---- Gap-aware TX scheduling ----
    // If we have gap prediction data, check whether the predicted gap is large
    // enough for the next request.  If not, defer (don't transmit into a gap
    // that's likely too small).
    GapPrediction gap = predictCurrentGap();
    bool gapPredictionAllows = true;

    if (gap.valid && !_requestQueue.empty()) {
        // Find the best request to fit in this gap (smallest wire time first)
        // This is checked AFTER we select sendIndex below, but we pre-compute
        // the prediction here so the log message is accurate.
        _gapPredictor.stats().predictionsUsed++;
    }

    // Pick the best request to send.
    // First TX (full silence wait): prefer short requests that fit in any gap.
    // Subsequent TXes (reduced silence): we're deep in a wrap-around gap,
    // so pick FIFO to ensure all registers get polled.
    // When silence is very long (>400ms), gap is definitely large → FIFO.
    uint32_t preCheckSilenceMs = (uint32_t)((micros() - _lastByteTime) / 1000ULL);
    size_t sendIndex = (size_t)-1;

    if (_ownTxInCurrentGap > 0 || preCheckSilenceMs >= 400) {
        // Subsequent TX or long silence: FIFO (all requests OK)
        for (size_t i = 0; i < _requestQueue.size(); i++) {
            if (!isUnitQueueingPaused(_requestQueue[i].unitId)) {
                sendIndex = i;
                break;
            }
        }
    } else {
        // First TX with short silence: pick smallest by wire time
        uint32_t bestWire = UINT32_MAX;
        for (size_t i = 0; i < _requestQueue.size(); i++) {
            if (!isUnitQueueingPaused(_requestQueue[i].unitId)) {
                uint32_t w = estimateWireTimeMs(_requestQueue[i].quantity);
                if (w < bestWire) {
                    bestWire = w;
                    sendIndex = i;
                }
            }
        }
    }
    
    // If bus is silent and we have no non-paused requests, allow ONE probe per unit
    // to check if the bus condition has improved. Limit probes to once per 250ms per unit
    // to detect recovery quickly while still protecting against hammering.
    if (sendIndex == (size_t)-1 && busSilent && !_requestQueue.empty()) {
        static constexpr uint32_t PROBE_INTERVAL_MS = 250;  // Probe every 250ms when bus is silent
        uint32_t now = millis();
        
        // Find first paused request whose unit hasn't been probed recently
        for (size_t i = 0; i < _requestQueue.size(); i++) {
            uint8_t unitId = _requestQueue[i].unitId;
            auto it = _backoffByUnit.find(unitId);
            if (it != _backoffByUnit.end()) {
                uint32_t timeSinceProbe = now - it->second.lastProbeAttemptMs;
                if (timeSinceProbe >= PROBE_INTERVAL_MS) {
                    sendIndex = i;
                    // Mark that we're probing this unit
                    it->second.lastProbeAttemptMs = now;
                    LOG_V("Probing paused unit %u (bus is silent, %ums since last probe)",
                          unitId, timeSinceProbe);
                    break;
                }
            }
        }
    }
    
    if (sendIndex == (size_t)-1) return;

    // Gap-aware TX decision.
    //
    // Two silence thresholds for gap-aware TX:
    // - MIN_GAP_SILENCE_MS (200ms): required before the FIRST TX in a gap.
    //   This ensures we're past inter-request gaps (~10-160ms) and into the
    //   wrap-around gap (~250-1800ms).
    // - MIN_INTER_TX_SILENCE_MS (10ms): used for subsequent TXes in the same
    //   gap.  After our TX+response, 10ms allows for bus settling and brief
    //   detection of any foreign activity starting.
    //
    // _ownTxInCurrentGap tracks consecutive own TXes.  Reset to 0 on foreign
    // activity; incremented after each successful own TX.
    //
    // Additional safety: UART RX buffer check immediately before TX.
    static constexpr uint32_t MIN_GAP_SILENCE_MS = 200;
    static constexpr uint32_t MIN_INTER_TX_SILENCE_MS = 70;

    // Don't TX until we've observed at least one foreign transaction.
    // At boot, _lastByteTime is from initialization and doesn't reflect
    // actual bus activity — the foreign master may be mid-cycle.
    if (!_hasLastTransactionEnd) return;

    uint32_t actualSilenceMs = (uint32_t)((micros() - _lastByteTime) / 1000ULL);
    uint32_t requiredSilenceMs = (_ownTxInCurrentGap > 0) ? MIN_INTER_TX_SILENCE_MS : MIN_GAP_SILENCE_MS;

    {
        uint32_t wireMs = estimateWireTimeMs(_requestQueue[sendIndex].quantity);

        if (actualSilenceMs < requiredSilenceMs) {
            _gapPredictor.stats().txDeferred++;
            return;
        }

        // Last-resort UART check: if bytes arrived between the silence
        // measurement above and now, a foreign frame is starting
        if (_serial.available() > 0) {
            _gapPredictor.stats().txDeferred++;
            LOG_V("TX deferred: UART RX bytes pending at decision point");
            return;
        }

        // Log TX-time context for post-mortem collision analysis
        uint32_t txElapsed = _gapWindowActive ? (uint32_t)(millis() - _gapWindowOpenMs) : 0;
        _lastTxElapsedMs = txElapsed;
        _lastTxWireMs = wireMs;
        _sentDuringGapWindow = gap.valid;
        LOG_I("Gap TX: silence=%ums(req=%u) wire=%ums qty=%u unit=%u gap=%s tx#=%u",
              actualSilenceMs, requiredSilenceMs, wireMs, _requestQueue[sendIndex].quantity,
              _requestQueue[sendIndex].unitId, gap.valid ? "yes" : "fallback",
              _ownTxInCurrentGap + 1);
    }

    _processQueueCounter++;
    _lastProcessQueueMs = millis();
    
    // Copy the selected request (not reference/pointer - prevents vector reallocation issues)
    ModbusPendingRequest req = _requestQueue[sendIndex];
    LOG_V("Processing request: Unit=%d, FC=0x%02X, Addr=%d, Qty=%d",
          req.unitId, req.functionCode, req.startRegister, req.quantity);
    
    if (sendRequest(req)) {
        LOG_V("Request sent successfully");
        _stats.ownRequestsSent++;
        _ownTxInCurrentGap++;  // subsequent TXes use shorter silence

        // Don't close the gap window after our TX: _lastByteTime is updated
        // by the response bytes, so the silence check naturally prevents
        // re-sending too quickly.  Keeping the window open allows multiple
        // TXes per wrap-around gap when it's long enough.

        // Gap scheduler stats
        uint32_t wireMs = estimateWireTimeMs(req.quantity);
        _gapPredictor.stats().totalWireTimeMs += wireMs;
        _gapPredictor.stats().lastTxMs = millis();
        _sentDuringGapWindow = gap.valid;
        if (gap.valid) {
            _gapPredictor.stats().txInGap++;
            _gapPredictor.stats().predictionsUsed++;
        } else {
            _gapPredictor.stats().txFallback++;
        }

        // Track our own FC3/FC4 requests in the register map
        uint8_t fc = req.functionCode & 0x7F;
        if (isReadFunction(fc)) {
            ModbusRegisterMap& map = ensureRegisterMap(req.unitId, fc);
            map.requestCount++;
            map.lastUpdate = millis();
        }
        
        // Store a COPY of the request, not a pointer into the vector
        // This prevents invalid references if vector reallocates
        _currentRequest = req;
        _hasPendingRequest = true;
        _waitingForResponse = true;
        _requestSentTime = millis();
        
        // Start tracking our active communication time
        startActiveTime(true);
        
        // Build the last request frame for response matching
        _lastRequest.unitId = req.unitId;
        _lastRequest.functionCode = req.functionCode;
        _lastRequest.dataLen = 4;
        _lastRequest.data[0] = (uint8_t)(req.startRegister >> 8);
        _lastRequest.data[1] = (uint8_t)(req.startRegister & 0xFF);
        _lastRequest.data[2] = (uint8_t)(req.quantity >> 8);
        _lastRequest.data[3] = (uint8_t)(req.quantity & 0xFF);

        // Also store under per-unit so FC3/FC4 response parsing can validate against our own requests.
        _lastRequest.timestamp = millis();
        _lastRequest.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
        _lastRequest.isRequest = true;
        _lastRequest.isValid = true;
        _lastRequest.isException = false;
        _lastRequest.exceptionCode = 0;
        _lastRequestPerUnit[req.unitId] = _lastRequest;

        // Disable stats gathering during our TX to avoid polluting foreign master patterns
        // Also clear the previous transaction context so the gap to the next foreign request
        // doesn't include our TX window
        _gapPredictor.setStatsEnabled(false);
        _lastTransactionEndMs = 0;
        _hasLastTransactionEnd = false;

        _requestQueue.erase(_requestQueue.begin() + (ptrdiff_t)sendIndex);
    } else {
        LOG_W("Failed to send request - bus not silent");
    }
}

bool ModbusRTUFeature::sendRequest(const ModbusPendingRequest& request) {

    
    // Use static TX buffer instead of heap allocation
    _txFrameLen = 0;
    
    _txFrameBuffer[_txFrameLen++] = request.unitId;
    _txFrameBuffer[_txFrameLen++] = request.functionCode;
    
    switch (request.functionCode) {
        case ModbusFC::READ_COILS:
        case ModbusFC::READ_DISCRETE_INPUTS:
        case ModbusFC::READ_HOLDING_REGISTERS:
        case ModbusFC::READ_INPUT_REGISTERS:
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister >> 8);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister & 0xFF);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.quantity >> 8);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.quantity & 0xFF);
            break;
            
        case ModbusFC::WRITE_SINGLE_REGISTER:
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister >> 8);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister & 0xFF);
            if (!request.writeData.empty()) {
                _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.writeData[0] >> 8);
                _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.writeData[0] & 0xFF);
            }
            break;
            
        case ModbusFC::WRITE_MULTIPLE_REGISTERS:
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister >> 8);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.startRegister & 0xFF);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.quantity >> 8);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.quantity & 0xFF);
            _txFrameBuffer[_txFrameLen++] = (uint8_t)(request.quantity * 2);  // Byte count
            for (uint16_t val : request.writeData) {
                if (_txFrameLen + 2 > TX_FRAME_BUFFER_SIZE - 2) break;  // Leave room for CRC
                _txFrameBuffer[_txFrameLen++] = (uint8_t)(val >> 8);
                _txFrameBuffer[_txFrameLen++] = (uint8_t)(val & 0xFF);
            }
            break;
            
        default:
            LOG_E("Unsupported function code: 0x%02X", request.functionCode);
            return false;
    }

    return sendFrameFromBuffer();
}

bool ModbusRTUFeature::sendFrameFromBuffer() {
    unsigned long txStartUs = micros();
    // Last-moment safety check: if bytes are already pending in the UART RX
    // buffer, a foreign frame is arriving (or just arrived).  Abort TX to
    // avoid colliding on the bus.
    int pending = _serial.available();
    if (pending > 0) {
        LOG_W("TX aborted: %d bytes pending in UART RX — foreign frame arriving", pending);
        _gapPredictor.stats().txDeferred++;
        return false;
    }

    // Calculate and append CRC
    uint16_t crc = calculateCRC(_txFrameBuffer, _txFrameLen);
    
    setDE(true);  // Enable transmitter
    delayMicroseconds(100);  // Small delay for transceiver
    
    for (size_t i = 0; i < _txFrameLen; i++) {
        _serial.write(_txFrameBuffer[i]);
    }
    _serial.write(crc & 0xFF);
    _serial.write(crc >> 8);
    
    _serial.flush();  // Wait for transmission complete
    delayMicroseconds(100);
    
    setDE(false);  // Back to receive mode

    uint32_t txUs = (uint32_t)(micros() - txStartUs);
    _lastLoopTiming.txUs = txUs;
    if (txUs > 0) {
        CpuMonitor::recordFeatureDuration("ModbusTX", txUs);
        recordDebugSample("ModbusTX", txUs);
    }

    // Back to receive mode and give transceiver time to settle
    // Mark end-of-TX as last bus activity for accurate silence detection.
    _lastByteTime = micros();

    // Drain any immediate RX bytes (transceiver echo) to avoid treating
    // our own echo as a foreign response. Limit drain to a reasonable cap.
    int drained = 0;
    const int DRAIN_CAP = 512;
    while (_serial.available() && drained < DRAIN_CAP) {
        (void)_serial.read();
        drained++;
    }
    if (drained > 0) {
        LOG_V("Drained %d RX bytes after TX (echo discard)", drained);
    }

    // Refresh empty-buffer tracking after TX and echo discard.
    if (_serial.available() == 0) {
        _serialWasEmpty = true;
        _serialEmptySinceUs = _lastByteTime;
    } else {
        _serialWasEmpty = false;
    }
    
    _stats.framesSent++;
    _lastActivityTime = millis();
    _busSilent = false;
    
    // Log TX with hex dump for debugging bus issues
    if (LOG_D_ACTIVE()) {
        char hexBuf[64];
        size_t hexLen = 0;
        size_t totalLen = _txFrameLen + 2; // +2 for CRC
        for (size_t i = 0; i < _txFrameLen && hexLen < 58; i++) {
            hexLen += snprintf(hexBuf + hexLen, sizeof(hexBuf) - hexLen, "%02X ", _txFrameBuffer[i]);
        }
        hexLen += snprintf(hexBuf + hexLen, sizeof(hexBuf) - hexLen, "%02X %02X", crc & 0xFF, crc >> 8);
        LOG_D("TX[%u]: %s", totalLen, hexBuf);
    }
    return true;
}

// Legacy sendFrame for sendRawFrame compatibility
void ModbusRTUFeature::sendFrame(const std::vector<uint8_t>& frame) {
    // Copy to static buffer and send
    _txFrameLen = (frame.size() < TX_FRAME_BUFFER_SIZE - 2) ? frame.size() : TX_FRAME_BUFFER_SIZE - 2;
    memcpy(_txFrameBuffer, frame.data(), _txFrameLen);
    (void)sendFrameFromBuffer();  // legacy path, ignore abort
}

bool ModbusRTUFeature::sendRawFrame(const uint8_t* data, size_t length) {
    if (!_busSilent) return false;
    // Copy directly to static TX buffer (avoids heap-allocating a vector)
    _txFrameLen = (length < TX_FRAME_BUFFER_SIZE - 2) ? length : TX_FRAME_BUFFER_SIZE - 2;
    memcpy(_txFrameBuffer, data, _txFrameLen);
    return sendFrameFromBuffer();
}

ModbusRegisterMap* ModbusRTUFeature::getRegisterMap(uint8_t unitId, uint8_t functionCode) {
    uint16_t key = makeMapKey(unitId, functionCode);
    auto it = _registerMaps.find(key);
    if (it != _registerMaps.end()) {
        return &it->second;
    }
    return nullptr;
}

bool ModbusRTUFeature::readCachedRegister(uint8_t unitId, uint8_t functionCode,
                                          uint16_t address, uint16_t& value) {
    ModbusRegisterMap* map = getRegisterMap(unitId, functionCode);
    if (!map) return false;
    
    auto it = map->registers.find(address);
    if (it != map->registers.end()) {
        value = it->second;
        return true;
    }
    return false;
}

bool ModbusRTUFeature::queueReadRegisters(uint8_t unitId, uint8_t functionCode,
                                          uint16_t startRegister, uint16_t quantity,
                                          std::function<void(bool, const ModbusFrame&)> callback) {
#if MODBUS_LISTEN_ONLY
    (void)unitId;
    (void)functionCode;
    (void)startRegister;
    (void)quantity;
    (void)callback;
    _stats.ownRequestsDiscarded++;
    return false;
#endif
    // Reject requests when suspended (e.g., during OTA)
    if (_suspended) {
        _stats.ownRequestsDiscarded++;
        return false;
    }
    
    // NOTE: Do not reject queueing during timeout backoff.
    // Backoff is enforced in processQueue() (sending), which prevents discard storms
    // and allows callers (web API, poll scheduler) to enqueue a probe request.
    
    // Check queue size
    if (_requestQueue.size() >= _maxQueueSize) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        LOG_E("Modbus request DISCARDED: queue full (%u/%u) - unit %d FC 0x%02X reg %d qty %d",
              _requestQueue.size(), _maxQueueSize, unitId, functionCode, startRegister, quantity);
        return false;
    }
    
    // Check memory pressure - if heap is critically low, don't queue
    uint32_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < 25000) {  // Less than 25 KB free - too risky
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
          const uint32_t minFree8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
          const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
          const uint32_t minFreeInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const uint32_t largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          LOG_E("Modbus request DISCARDED: critical heap (free8=%u min8=%u largest8=%u freeInt=%u minInt=%u largestInt=%u) - unit %d FC 0x%02X",
              freeHeap, minFree8, largest8,
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), minFreeInt, largestInt,
              unitId, functionCode);
        return false;
    }
    
    ModbusPendingRequest req;
    req.unitId = unitId;
    req.functionCode = functionCode;
    req.startRegister = startRegister;
    req.quantity = quantity;
    req.callback = callback;
    req.queuedAt = millis();
    req.retries = 0;
    
    _requestQueue.push_back(req);
    return true;
}

bool ModbusRTUFeature::queueWriteSingleRegister(uint8_t unitId, uint16_t address, uint16_t value,
                                                 std::function<void(bool, const ModbusFrame&)> callback) {
#if MODBUS_LISTEN_ONLY
    (void)unitId;
    (void)address;
    (void)value;
    (void)callback;
    _stats.ownRequestsDiscarded++;
    return false;
#endif
    // Reject requests when suspended (e.g., during OTA)
    if (_suspended) {
        _stats.ownRequestsDiscarded++;
        return false;
    }
    
    // NOTE: Do not reject queueing during timeout backoff; backoff is enforced on sending.
    
    // Check queue size
    if (_requestQueue.size() >= _maxQueueSize) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        LOG_E("Modbus write request DISCARDED: queue full (%u/%u) - unit %d reg %d value %d",
              _requestQueue.size(), _maxQueueSize, unitId, address, value);
        return false;
    }
    
    // Check memory pressure
    uint32_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < 25000) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
          const uint32_t minFree8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
          const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
          const uint32_t minFreeInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const uint32_t largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          LOG_E("Modbus write request DISCARDED: critical heap (free8=%u min8=%u largest8=%u freeInt=%u minInt=%u largestInt=%u) - unit %d reg %d",
              freeHeap, minFree8, largest8,
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), minFreeInt, largestInt,
              unitId, address);
        return false;
    }
    
    ModbusPendingRequest req;
    req.unitId = unitId;
    req.functionCode = ModbusFC::WRITE_SINGLE_REGISTER;
    req.startRegister = address;
    req.quantity = 1;
    req.writeData.push_back(value);
    req.callback = callback;
    req.queuedAt = millis();
    req.retries = 0;
    
    _requestQueue.push_back(req);
    return true;
}

bool ModbusRTUFeature::queueWriteMultipleRegisters(uint8_t unitId, uint16_t startAddress,
                                                    const std::vector<uint16_t>& values,
                                                    std::function<void(bool, const ModbusFrame&)> callback) {
#if MODBUS_LISTEN_ONLY
    (void)unitId;
    (void)startAddress;
    (void)values;
    (void)callback;
    _stats.ownRequestsDiscarded++;
    return false;
#endif
    // Reject requests when suspended (e.g., during OTA)
    if (_suspended) {
        _stats.ownRequestsDiscarded++;
        return false;
    }
    
    // NOTE: Do not reject queueing during timeout backoff; backoff is enforced on sending.
    
    // Check queue size
    if (_requestQueue.size() >= _maxQueueSize) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        LOG_E("Modbus write-multi request DISCARDED: queue full (%u/%u) - unit %d reg %d count %u",
              _requestQueue.size(), _maxQueueSize, unitId, startAddress, values.size());
        return false;
    }
    
    // Check memory pressure
    uint32_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < 25000) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
          const uint32_t minFree8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
          const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
          const uint32_t minFreeInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const uint32_t largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          LOG_E("Modbus write-multi request DISCARDED: critical heap (free8=%u min8=%u largest8=%u freeInt=%u minInt=%u largestInt=%u) - unit %d",
              freeHeap, minFree8, largest8,
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), minFreeInt, largestInt,
              unitId);
        return false;
    }
    
    ModbusPendingRequest req;
    req.unitId = unitId;
    req.functionCode = ModbusFC::WRITE_MULTIPLE_REGISTERS;
    req.startRegister = startAddress;
    req.quantity = values.size();
    req.writeData = values;
    req.callback = callback;
    req.queuedAt = millis();
    req.retries = 0;
    
    _requestQueue.push_back(req);
    return true;
}

void ModbusRTUFeature::setDE(bool transmit) {
    if (_dePin >= 0) {
        digitalWrite(_dePin, transmit ? HIGH : LOW);
    }
}

uint16_t ModbusRTUFeature::calculateCRC(const uint8_t* data, size_t length) const {
    // Delegate to testable, shared implementation
    return modbus_crc16(data, length);
}

void ModbusRTUFeature::startActiveTime(bool isOwn) {
    if (!_inActiveTime) {
        _inActiveTime = true;
        _activeTimeIsOwn = isOwn;
        _activeStartTimeUs = micros();
    }
}

void ModbusRTUFeature::endActiveTime() {
    if (_inActiveTime) {
        unsigned long duration = micros() - _activeStartTimeUs;
        if (_activeTimeIsOwn) {
            _stats.ownActiveTimeUs += duration;
            _intervalStats.ownActiveTimeUs += duration;
        } else {
            _stats.otherActiveTimeUs += duration;
            _intervalStats.otherActiveTimeUs += duration;
        }
        _inActiveTime = false;
    }
}

float ModbusRTUFeature::getOwnFailureRate() const {
    uint32_t total = _stats.ownRequestsSuccess + _stats.ownRequestsFailed;
    if (total == 0) return 0.0f;
    return (float)_stats.ownRequestsFailed / (float)total;
}

float ModbusRTUFeature::getBusIdlePercent() const {
    if (_stats.totalTimeUs == 0) return 100.0f;
    uint64_t activeTimeUs = _stats.ownActiveTimeUs + _stats.otherActiveTimeUs;
    uint64_t idleTimeUs = (_stats.totalTimeUs > activeTimeUs) ? 
                          (_stats.totalTimeUs - activeTimeUs) : 0;
    return (float)idleTimeUs * 100.0f / (float)_stats.totalTimeUs;
}

void ModbusRTUFeature::resetStats() {
    memset(&_stats, 0, sizeof(_stats));
    _stats.lastStatsReset = millis();
    resetBusPatterns();
}

void ModbusRTUFeature::resetIntervalStats() {
    memset(&_intervalStats, 0, sizeof(_intervalStats));
    _intervalStats.intervalStartMs = millis();
}

float ModbusRTUFeature::getOtherFailureRate() const {
    uint32_t total = _intervalStats.otherSuccess + _intervalStats.otherFailed;
    if (total == 0) return 0.0f;
    return (float)_intervalStats.otherFailed / (float)total;
}

void ModbusRTUFeature::checkAndLogWarnings() {
    // Calculate interval time in microseconds
    unsigned long intervalMs = millis() - _intervalStats.intervalStartMs;
    uint64_t intervalUs = (uint64_t)intervalMs * 1000ULL;
    
    // Track interval stats
    uint32_t ownTotal = _intervalStats.ownSuccess + _intervalStats.ownFailed;
    uint32_t otherTotal = _intervalStats.otherSuccess + _intervalStats.otherFailed;
    
    // Check own request failure rate (configurable threshold, default 5%)
    if (ownTotal >= 10) {  // Need at least 10 requests for meaningful percentage
        float failureRate = (float)_intervalStats.ownFailed / (float)ownTotal;
        float thresholdPercent = (float)MODBUS_OWN_FAIL_WARN_PERCENT;
        if (failureRate * 100.0f > thresholdPercent) {
            LOG_W("Modbus own request failure rate: %.1f%% (%u/%u failed in last %lus)",
                  failureRate * 100.0f, 
                  _intervalStats.ownFailed, ownTotal,
                  intervalMs / 1000);
        }
    }
    
    // Check other device failure rate (configurable threshold, default 5%)
    if (otherTotal >= 10) {  // Need at least 10 responses for meaningful percentage
        float failureRate = getOtherFailureRate();
        float thresholdPercent = (float)MODBUS_OTHER_FAIL_WARN_PERCENT;
        if (failureRate * 100.0f > thresholdPercent) {
            LOG_W("Modbus other device failure rate: %.1f%% (%u/%u failed in last %lus)",
                  failureRate * 100.0f,
                  _intervalStats.otherFailed, otherTotal,
                  intervalMs / 1000);
        }
    }
    
    // Check bus utilization (configurable threshold, default 95% busy = 5% idle)
    if (intervalUs > 10000000) {  // At least 10 seconds of data
        uint64_t activeTimeUs = _intervalStats.ownActiveTimeUs + _intervalStats.otherActiveTimeUs;
        float busyPercent = (float)activeTimeUs * 100.0f / (float)intervalUs;
        float thresholdPercent = (float)MODBUS_BUS_BUSY_WARN_PERCENT;
        if (busyPercent > thresholdPercent) {
            float ownPercent = (float)_intervalStats.ownActiveTimeUs * 100.0f / (float)intervalUs;
            float otherPercent = (float)_intervalStats.otherActiveTimeUs * 100.0f / (float)intervalUs;
            LOG_W("Modbus bus utilization high: busy=%.1f%% (own=%.1f%%, other=%.1f%%) in last %lus",
                  busyPercent, ownPercent, otherPercent, intervalMs / 1000);
        }
    }
    
    // Log summary at INFO level (using cumulative stats)
    unsigned long uptimeSec = (millis() - _stats.lastStatsReset) / 1000;
    uint32_t totalOwn = _stats.ownRequestsSuccess + _stats.ownRequestsFailed;
    if (uptimeSec > 0 && totalOwn > 0) {
          const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
          const uint32_t minFree8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
          const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
          const uint32_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const uint32_t minFreeInt = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const uint32_t largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          const float frag8Pct = (free8 > 0) ? (100.0f - ((float)largest8 * 100.0f / (float)free8)) : 0.0f;

          LOG_I("Modbus stats (%lus): own=%u/%u ok, other=%u req, CRC=%u, idle=%.1f%%, heap8=%u(min=%u,largest=%u,frag=%.0f%%) heapInt=%u(min=%u,largest=%u)",
              uptimeSec,
              _stats.ownRequestsSuccess, totalOwn,
              _stats.otherRequestsSeen,
              _stats.crcErrors,
              getBusIdlePercent(),
              free8, minFree8, largest8, frag8Pct,
              freeInt, minFreeInt, largestInt);
    }
    
    // Reset interval stats for next period
    resetIntervalStats();
}

String ModbusRTUFeature::formatHex(const uint8_t* data, size_t length) const {
    std::string s = format_hex(data, length);
    return String(s.c_str());
}

// ===== Bus Pattern Analysis =====

constexpr uint32_t BusGapStats::kBoundariesUs[BusGapStats::NUM_BUCKETS];

void ModbusRTUFeature::onFrameBoundary(size_t bytesInBuffer) {
    // Called every time processReceivedData() fires (3.5 char-time or 1.5 char-time gap).
    // `_rxBufferStartUs` is the micros() timestamp of the first byte in the current buffer.
    
    _busByteStats.totalBytes += (uint32_t)bytesInBuffer;
    _busByteStats.totalFrameBoundaries++;
    _busByteStats.lastUpdateMs = millis();

    // Record inter-frame gap: silence between end of previous frame chunk and start of this one.
    // Previous frame ended at _lastFrameBoundaryUs (last byte).
    // This frame started at _rxBufferStartUs (first byte).
    if (_hasLastFrameBoundary && _rxBufferStartUs > _lastFrameBoundaryUs) {
        uint32_t gapUs = (uint32_t)(_rxBufferStartUs - _lastFrameBoundaryUs);
        recordBusGap(gapUs);
    }

    // Store end-of-current-chunk for next gap calculation.
    // The last byte of this chunk was received at _lastByteTime (micros).
    _lastFrameBoundaryUs = (unsigned long)_lastByteTime;
    _hasLastFrameBoundary = true;
}

void ModbusRTUFeature::recordBusPattern(const ModbusFrame& frame) {
    const uint32_t minInterframeCalc = (uint32_t)(3.5 * _charTimeUs / 1000.0) + 1;
    const uint32_t minInterframeMs = (minInterframeCalc < 10) ? 10 : minInterframeCalc;

    auto res = _patternTracker.recordFrame(frame, minInterframeMs,
                                           _hasLastCompletedTx, _lastCompletedTxKey, _lastTransactionEndMs);
    if (res.hasTransition) {
        _gapPredictor.recordTransition(res.predecessorKey, res.successorKey, res.gapMs);
    }
}

void ModbusRTUFeature::recordBusGap(uint32_t gapUs) {
    _busGapStats.record(gapUs);
}

// ---- Gap-aware TX scheduling ----

GapPrediction ModbusRTUFeature::predictCurrentGap() const {
    // Delegate to GapPredictor using the last completed transaction key
    if (!_hasLastCompletedTx) return GapPrediction{};
    return _gapPredictor.predictCurrentGap(_lastCompletedTxKey);
}

uint32_t ModbusRTUFeature::estimateWireTimeMs(uint16_t quantity) const {
    // Request: 8 bytes (unit + FC + startReg_hi/lo + qty_hi/lo + CRC_lo/hi)
    // Response: 5 + 2*qty bytes (unit + FC + byteCount + data + CRC)
    // + ~5ms device turnaround
    uint32_t totalBytes = 8 + 5 + 2 * (uint32_t)quantity;
    uint32_t wireMs = (uint32_t)((uint64_t)totalBytes * _charTimeUs / 1000ULL);
    wireMs += 5;  // device turnaround
    return wireMs;
}

// reportCollision now handled by GapPredictor::reportCollision

void ModbusRTUFeature::resetBusPatterns() {
    _patternTracker.reset();
    _busGapStats.reset();
    _busByteStats.reset();
    _busTransactionStats.reset();
    _hasLastFrameBoundary = false;
    _lastFrameBoundaryUs = 0;
    _lastTransactionEndMs = 0;
    _hasLastTransactionEnd = false;
    _lastTransactionEndMs = 0;
    _hasLastTransactionEnd = false;
    _lastCompletedTxKey = 0;
    _hasLastCompletedTx = false;
    _gapPredictor.reset();
    _gapWindowActive = false;
    _gapWindowBudgetMs = 0;
    _gapWindowUsedMs = 0;
    _sentDuringGapWindow = false;
    // cycle sequence reset performed inside pattern tracker
    LOG_I("Bus pattern tracking reset");
}

void ModbusRTUFeature::detectCycle() {
    _patternTracker.detectCycle();
}
