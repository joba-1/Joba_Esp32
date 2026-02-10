#include "ModbusRTUFeature.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include "TimeUtils.h"

static inline bool timeBefore32(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
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

ModbusRTUFeature::ModbusRTUFeature(HardwareSerial& serial,
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
    // unit + fc + payload + crc(2)
    String result;
    result.reserve(3 * (2 + frame.dataLen + 2));

    auto appendByte = [&](uint8_t b) {
        if (result.length() > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", b);
        result += buf;
    };

    appendByte(frame.unitId);
    appendByte(frame.functionCode);
    for (size_t i = 0; i < frame.dataLen; i++) appendByte(frame.data[i]);
    appendByte((uint8_t)(frame.crc & 0xFF));
    appendByte((uint8_t)((frame.crc >> 8) & 0xFF));
    return result;
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
    _gapSchedulerStats.startMs = millis();
    _ready = true;
}

void ModbusRTUFeature::loop() {
    if (!_ready) return;
    
    // When suspended, skip all processing (OTA in progress)
    if (_suspended) return;

    _loopCounter++;
    
    unsigned long nowUs = micros();
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
        processReceivedData();
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
    
    // Check for response timeout
    if (_waitingForResponse && (nowMs - _requestSentTime) > _responseTimeoutMs) {
        _stats.timeouts++;
        _stats.ownRequestsFailed++;
        _intervalStats.ownFailed++;
        
        // Throttle individual timeout messages - log at most once per 5 seconds per unit
        // This prevents timeout spam while still providing visibility
        uint16_t unitKey = _lastRequest.unitId;
        unsigned long lastLog = _lastTimeoutPerUnit[unitKey];
        if ((nowMs - lastLog) >= 5000) {  // 5 second throttle per unit
            LOG_W("Modbus response timeout for unit %d FC 0x%02X reg %d qty %d",
                  _lastRequest.unitId, _lastRequest.functionCode,
                  _lastRequest.getStartRegister(), _lastRequest.getQuantity());
            _lastTimeoutPerUnit[unitKey] = nowMs;
        }
        
        // Track consecutive timeouts per unit to trigger backoff (do not globally pause other units).
        const uint8_t unitId = _currentRequest.unitId;
        TimeoutBackoffState& st = _backoffByUnit[unitId];
        st.consecutiveTimeouts++;
        if (st.consecutiveTimeouts >= 3) {
            st.pausedUntilMs = (uint32_t)nowMs + st.backoffMs;
            if (st.consecutiveTimeouts == 3) {
                LOG_W("Modbus: 3 consecutive timeouts for unit %u, pausing sends for %ums", unitId, st.backoffMs);
            }
            // Exponential backoff, capped at 60s.
            if (st.backoffMs < 60000) {
                st.backoffMs = std::min<uint32_t>(st.backoffMs * 2, 60000);
            }
        }
        
        // Invoke callback on timeout so callers (e.g. tracked raw reads) know the request failed.
        // Copy callback before clearing state to avoid use-after-clear.
        std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
        if (_currentRequest.callback) {
            callbackCopy = _currentRequest.callback;
        }
        
        // If we sent this request using gap prediction and it timed out,
        // it likely collided with the foreign master — increase safety margin
        if (_sentDuringGapWindow) {
            reportCollision();
            _sentDuringGapWindow = false;
        }

        _waitingForResponse = false;
        _hasPendingRequest = false;
        endActiveTime();
        
        // Call callback outside critical section with an empty frame indicating timeout
        if (callbackCopy) {
            ModbusFrame emptyFrame;
            emptyFrame.isValid = false;
            emptyFrame.isException = false;
            try {
                callbackCopy(false, emptyFrame);
            } catch (...) {
                LOG_E("Exception in Modbus timeout callback");
            }
        }
        
        // If the queue is building up, drop requests for the timed-out unit only.
        // This prevents one unresponsive unit from starving other devices.
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
            processQueue(true);  // Bus is silent, allow probing backoff units
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
                    processQueue(true);  // Bus is silent, allow probing backoff units
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
}

void ModbusRTUFeature::processReceivedData() {
    if (_rxBuffer.size() < 4) {
        // Still record byte-level stats even for short/incomplete frames
        if (_rxBuffer.size() > 0) {
            onFrameBoundary(_rxBuffer.size());
        }
        // Incomplete frames are common on noisy buses; don't spam logs
        _rxBuffer.clear();
        return;
    }

    onFrameBoundary(_rxBuffer.size());

    // Spec-based extraction for Modbus RTU (focus: FC3/FC4).
    // For FC3/FC4 we can deduce the exact frame length from the function code and (for responses) byteCount.
    // This drastically reduces false-positive "valid" frames compared to CRC-scanning arbitrary slices.
    static constexpr uint8_t FC3 = ModbusFC::READ_HOLDING_REGISTERS;
    static constexpr uint8_t FC4 = ModbusFC::READ_INPUT_REGISTERS;
    static constexpr uint8_t FC3_EX = (uint8_t)(FC3 | 0x80);
    static constexpr uint8_t FC4_EX = (uint8_t)(FC4 | 0x80);
    static constexpr uint8_t MAX_RTU_UNIT_ID = 247;
    static constexpr uint16_t MAX_REGS_PER_READ = 125;
    static constexpr uint8_t MAX_BYTECOUNT = 250;  // per spec: max 125 regs => 250 bytes

    size_t i = 0;
    size_t extractedCount = 0;
    bool sawNoise = false;

    while (i + 4 <= _rxBuffer.size()) {
        const uint8_t* p = _rxBuffer.data() + i;
        size_t remaining = _rxBuffer.size() - i;

        uint8_t unitId = p[0];
        uint8_t fc = p[1];

        // Basic plausibility: this is a sniffer; broadcast (0) is not useful here.
        // Reject unitId=0 to drastically reduce false positives on noisy/contended buses.
        if (unitId == 0 || unitId > MAX_RTU_UNIT_ID) {
            sawNoise = true;
            i++;
            continue;
        }

        bool isRequest = false;
        size_t frameLen = 0;
        ModbusFrame frame;

        enum class TryParseResult : uint8_t { Fail = 0, Valid = 1, CrcInvalid = 2 };
        auto tryParseAtLen = [&](size_t len) -> TryParseResult {
            if (remaining < len) return TryParseResult::Fail;
            if (!parseFrame(p, len, frame)) return TryParseResult::Fail;
            return frame.isValid ? TryParseResult::Valid : TryParseResult::CrcInvalid;
        };

        // Exceptions for FC3/FC4 are fixed length: unit + fc|0x80 + excCode + crc(2) = 5
        if ((fc == FC3_EX || fc == FC4_EX) && remaining >= 5) {
            const TryParseResult r = tryParseAtLen(5);
            if (r != TryParseResult::Fail && frame.isException) {
                // Exception responses are responses (never requests)
                isRequest = false;
                frameLen = 5;
            }
        }

        // Normal FC3/FC4
        if (frameLen == 0 && (fc == FC3 || fc == FC4)) {
            // IMPORTANT: Try request FIRST (fixed 8 bytes).
            // Many real-world register addresses start with an even MSB (e.g. 0x06xx), which can look like
            // a valid response byteCount and cause false-positive response parsing if we try response first.
            if (remaining >= 8) {
                const TryParseResult r = tryParseAtLen(8);
                if (r != TryParseResult::Fail && !frame.isException && frame.dataLen == 4) {
                    uint16_t qty = frame.getQuantity();
                    if (qty >= 1 && qty <= MAX_REGS_PER_READ) {
                        isRequest = true;
                        frameLen = 8;
                    }
                }
            }

            // Try response: unit + fc + byteCount + data + crc
            if (frameLen == 0 && remaining >= 5) {
                uint8_t byteCount = p[2];
                // Spec: byteCount must be even for register reads and <= 250.
                if (byteCount >= 2 && (byteCount % 2) == 0 && byteCount <= MAX_BYTECOUNT) {
                    size_t respLen = (size_t)byteCount + 5;
                    const TryParseResult r = tryParseAtLen(respLen);
                    if (r != TryParseResult::Fail && !frame.isException) {
                        // Optional stronger validation using last seen request for this unit/fc
                        // Prefer our in-flight request (if any) over sniffed traffic.
                        // This avoids foreign masters overwriting _lastRequestPerUnit and causing us
                        // to incorrectly discard our own response as "noise" due to byteCount mismatch.
                        const uint8_t inflightFc = (uint8_t)(_currentRequest.functionCode & 0x7F);
                        if (_waitingForResponse && _hasPendingRequest && unitId == _currentRequest.unitId && inflightFc == fc) {
                            const uint16_t qty = _currentRequest.quantity;
                            if (qty >= 1 && qty <= MAX_REGS_PER_READ) {
                                if ((size_t)byteCount != (size_t)qty * 2) {
                                    sawNoise = true;
                                    i++;
                                    continue;
                                }
                            }
                        } else {
                            auto reqIt = _lastRequestPerUnit.find(unitId);
                            if (reqIt != _lastRequestPerUnit.end()) {
                                const ModbusFrame& req = reqIt->second;
                                uint8_t reqFc = req.functionCode & 0x7F;
                                if (req.isValid && reqFc == fc && req.dataLen == 4) {
                                    // Only enforce if response is reasonably close in time
                                    if ((frame.timestamp - req.timestamp) < 2000) {
                                        uint16_t qty = req.getQuantity();
                                        if (qty >= 1 && qty <= MAX_REGS_PER_READ) {
                                            if ((size_t)byteCount != (size_t)qty * 2) {
                                                // Mismatched byte count => not a valid FC3/FC4 response for the request we saw.
                                                // Treat as noise and keep searching.
                                                sawNoise = true;
                                                i++;
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        isRequest = false;
                        frameLen = respLen;
                    }
                }
            }
        }

        // Not supported / not implemented
        if (frameLen == 0) {
            // Discard FCs we don't implement for now.
            // FCs outside the Modbus spec are treated as noise.
            // FCs in-spec but unimplemented are also discarded (future extension point).
            // (We don't spam logs here; higher-level stats already track CRC noise.)
            sawNoise = true;
            i++;
            continue;
        }

          // At this point we have a spec-plausible frame. It may be CRC-valid or CRC-invalid.
          extractedCount++;

          // Best-effort start-of-message uptime:
          // - if this RX buffer was built from multiple frames, "i" approximates the offset.
          // - works well when frames are contiguous in the buffer.
          const uint32_t approxStartMs = _rxBufferStartMs + (uint32_t)((uint64_t)i * (uint64_t)_charTimeUs / 1000ULL);
          frame.timestamp = approxStartMs;
          frame.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
          frame.isRequest = isRequest;

          if (!frame.isValid) {
            // Only count CRC error if this is the first bad frame (not during resync scanning)
            if (!_inResync) {
                _stats.crcErrors++;
                _busByteStats.invalidFrames++;
                LOG_W("RX Frame (CRC ERROR): Unit=%d, FC=0x%02X, Raw=%s",
                    frame.unitId, frame.functionCode,
                    formatFrameHex(frame).c_str());

                // Close the gap window: a CRC error means something is
                // being transmitted on the bus (likely a collision).
                // Keeping the gap open after a collision causes cascading
                // collisions as we keep transmitting in a stale window.
                _gapWindowActive = false;
            } else {
                LOG_V("RX resync attempt: Unit=%d, FC=0x%02X (not counted)", 
                    frame.unitId, frame.functionCode);
            }
            recordFrameToHistory(frame);
            if (_frameCallback) {
                _frameCallback(frame, isRequest);
            }
            // Advance by only 1 byte on CRC errors to allow frame resynchronization
            // (the calculated frameLen is unreliable when parsing started at wrong offset)
            _inResync = true;  // Enter resync mode
            i++;
            continue;
          }

          // CRC-valid frame - exit resync mode
          _inResync = false;
          _stats.framesReceived++;
          _busByteStats.validFrames++;

          // Frame details available via /api/modbus/monitor; don't spam logs
          recordFrameToHistory(frame);

        bool isOurResponse = false;

        // Match our response more strictly:
        // - must be a response (not a request)
        // - unitId + functionCode (or exception variant)
        // - for FC3/FC4: response byteCount must match our requested quantity
        const uint8_t expectedFc = _currentRequest.functionCode;
        const uint8_t expectedFcBase = (uint8_t)(expectedFc & 0x7F);
        const bool fcMatches = (frame.functionCode == expectedFc) ||
                               (frame.isException && ((frame.functionCode & 0x7F) == expectedFcBase));

        bool byteCountMatches = true;
        if (!frame.isException && (expectedFcBase == FC3 || expectedFcBase == FC4)) {
            const size_t expectedBytes = (size_t)_currentRequest.quantity * 2;
            const size_t actualBytes = frame.getByteCount();
            byteCountMatches = (actualBytes == expectedBytes);
        }

        if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
            !frame.isRequest && frame.unitId == _currentRequest.unitId &&
            fcMatches && byteCountMatches) {
            isRequest = false;
            isOurResponse = true;
            _waitingForResponse = false;

            // Record our own round-trip time
            {
                uint32_t rtt = (uint32_t)(millis() - _requestSentTime);
                _busTransactionStats.record(rtt);
            }

            // Reset backoff for this unit only
            _backoffByUnit.erase(frame.unitId);
            _lastSuccessTime = millis();

            if (!frame.isException) {
                _stats.ownRequestsSuccess++;
                _intervalStats.ownSuccess++;

                // Count registers read on successful response
                _gapSchedulerStats.registersRead += _currentRequest.quantity;

                // Gap scheduler: successful TX during predicted gap
                if (_sentDuringGapWindow) {
                    _gapSchedulerStats.gapSufficient++;
                    // Gradually relax safety margin after sustained success
                    if (_gapSchedulerStats.gapSufficient % GAP_RELAX_INTERVAL == 0 &&
                        _gapSchedulerStats.safetyMargin > _gapSchedulerStats.minMargin) {
                        float oldMargin = _gapSchedulerStats.safetyMargin;
                        _gapSchedulerStats.safetyMargin = std::max(
                            _gapSchedulerStats.minMargin,
                            _gapSchedulerStats.safetyMargin - 0.01f);
                        LOG_I("Gap scheduler: margin relaxed %.0f%% -> %.0f%% after %u successful gap TXes",
                              oldMargin * 100.0f, _gapSchedulerStats.safetyMargin * 100.0f,
                              _gapSchedulerStats.gapSufficient);
                    }
                }
                _sentDuringGapWindow = false;
            } else {
                _stats.ownRequestsFailed++;
                _intervalStats.ownFailed++;
                _sentDuringGapWindow = false;
                LOG_W("Modbus exception 0x%02X from unit %d",
                      frame.exceptionCode, frame.unitId);
            }

            updateRegisterMap(_lastRequest, frame);

            std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
            if (_currentRequest.callback) {
                callbackCopy = _currentRequest.callback;
            }
            _hasPendingRequest = false;

            if (callbackCopy) {
                try {
                    callbackCopy(!frame.isException, frame);
                } catch (...) {
                    LOG_E("Exception in Modbus response callback");
                }
            }

            endActiveTime();

            // Track end of our transaction for cycle gap measurement
            _lastTransactionEndMs = millis();
            _hasLastTransactionEnd = true;

        } else if (_waitingForResponse && _hasPendingRequest && frame.isValid && !frame.isRequest) {
            // Debug: why didn't this match our request?
            // Record mismatch for diagnostics
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
        } else {
            // Foreign traffic (sniffed)
            if (isRequest) {
                // Some RS485 transceivers/UART setups echo our own transmitted bytes back into RX.
                // If we are currently waiting for a response, and this request exactly matches
                // the in-flight request, treat it as TX echo and do not feed it into the passive
                // request/response tracking.
                if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
                    frame.unitId == _currentRequest.unitId &&
                    ((frame.functionCode & 0x7F) == (_currentRequest.functionCode & 0x7F)) &&
                    (frame.getStartRegister() == _currentRequest.startRegister) &&
                    (frame.getQuantity() == _currentRequest.quantity)) {
                    // Echo is typically immediate; still accept it regardless of exact timing.
                    // We already keep _lastRequestPerUnit for our own requests in processQueue().
                    // Do not count this as other traffic.
                } else {
                // FC3/FC4: track per-unit requests and register-map requestCount
                uint8_t reqFc = frame.functionCode & 0x7F;
                if (reqFc == FC3 || reqFc == FC4) {
                    ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, reqFc);
                    map.requestCount++;
                    map.lastUpdate = millis();
                }

                _lastRequestPerUnit[frame.unitId] = frame;
                _stats.otherRequestsSeen++;

                // Bus pattern tracking: record request timing
                recordBusPattern(frame);
                
                // Multi-master arbitration: mark that we saw a foreign request, 
                // so we wait for its response before transmitting
                _sawForeignRequest = true;
                _foreignRequestTimeMs = millis();

                // Close any open gap window — bus is active again
                _gapWindowActive = false;
                
                startActiveTime(false);
                }
            } else {
                if (frame.isException) {
                    _stats.otherExceptionsSeen++;
                    _intervalStats.otherFailed++;
                    
                    // Foreign response received - clear the "waiting for response" flag
                    _sawForeignRequest = false;

                    // Pairing quality (best-effort): try to associate exception with a recent request
                    // from the same unit and matching FC.
                    {
                        uint8_t exFc = frame.functionCode & 0x7F;
                        bool paired = false;
                        if (exFc == FC3 || exFc == FC4) {
                            auto reqIt = _lastRequestPerUnit.find(frame.unitId);
                            if (reqIt != _lastRequestPerUnit.end()) {
                                const ModbusFrame& req = reqIt->second;
                                if (req.isValid && ((req.functionCode & 0x7F) == exFc) && req.dataLen == 4) {
                                    if ((frame.timestamp - req.timestamp) < 2000) {
                                        paired = true;
                                    }
                                }
                            }

                            if (paired) {
                                _stats.otherExceptionsPaired++;
                            } else {
                                _stats.otherExceptionsUnpaired++;
                            }
                        }
                    }

                    // FC3/FC4 exceptions: count as device/map errors
                    uint8_t exFc = frame.functionCode & 0x7F;
                    if (exFc == FC3 || exFc == FC4) {
                        ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, exFc);
                        map.responseCount++;
                        map.errorCount++;
                        map.lastUpdate = millis();
                    }
                } else {
                    _stats.otherResponsesSeen++;
                    _intervalStats.otherSuccess++;
                    
                    // Foreign response received - clear the "waiting for response" flag
                    _sawForeignRequest = false;
                }

                if (!frame.isException) {
                    auto reqIt = _lastRequestPerUnit.find(frame.unitId);
                    bool updated = false;
                    if (reqIt != _lastRequestPerUnit.end()) {
                        const ModbusFrame& req = reqIt->second;
                        if (req.isValid && ((req.functionCode & 0x7F) == (frame.functionCode & 0x7F)) && req.dataLen == 4) {
                            if ((frame.timestamp - req.timestamp) < 2000) {
                                updateRegisterMap(req, frame);
                                updated = true;
                                // Record observed transaction time (request start → response end)
                                // = request TX + device turnaround + response TX
                                uint32_t respEndMs = frame.timestamp + (uint32_t)((uint64_t)frameLen * _charTimeUs / 1000ULL);
                                uint32_t rtt = (uint32_t)(respEndMs - req.timestamp);
                                _busTransactionStats.record(rtt);

                                // Remember last completed transaction for successor gap tracking
                                uint16_t startReg = (req.data[0] << 8) | req.data[1];
                                uint16_t qty      = (req.data[2] << 8) | req.data[3];
                                _lastCompletedTxKey = makeBusPatternKey(req.unitId, req.functionCode, startReg, qty);
                                _hasLastCompletedTx = true;

                                // Keep _lastTransactionEndMs in sync with _lastCompletedTxKey.
                                // Both must refer to the same transaction so that successor gap
                                // and transition gap measurements are attributed correctly.
                                // (Previously this was set unconditionally for all responses,
                                // causing unpaired responses to desync the two values and pollute
                                // the transition map with near-zero gap artefacts.)
                                _lastTransactionEndMs = respEndMs;
                                _hasLastTransactionEnd = true;
                            }
                        }
                    }

                    // If we couldn't map it (e.g., request not observed), still count the response.
                    uint8_t respFc = frame.functionCode & 0x7F;
                    if (!updated && (respFc == FC3 || respFc == FC4)) {
                        ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, respFc);
                        map.responseCount++;
                        map.lastUpdate = millis();
                    }

                    // Pairing quality counters for FC3/FC4 responses
                    {
                        uint8_t respFc = frame.functionCode & 0x7F;
                        if (respFc == FC3 || respFc == FC4) {
                            if (updated) {
                                _stats.otherResponsesPaired++;
                            } else {
                                _stats.otherResponsesUnpaired++;
                            }
                        }
                    }

                    // Open gap window for gap-aware TX scheduling.
                    // Only open on PAIRED responses where we successfully matched
                    // request→response — this ensures _lastCompletedTxKey accurately
                    // reflects the just-completed transaction.  Unpaired responses
                    // (where we missed the request, e.g., due to a bus collision)
                    // would produce a stale predecessor key and wrong predictions.
                    if (updated && _hasLastCompletedTx) {
                        _gapWindowOpenMs = millis();
                        _gapWindowActive = true;
                        _gapWindowUsedMs = 0;
                    }
                }
            }
        }

        if (_frameCallback) {
            _frameCallback(frame, isRequest);
        }

        i += frameLen;
    }

    // If there were leftover bytes that didn't form any frame, count as CRC/noise once.
    if ((i < _rxBuffer.size()) || (sawNoise && extractedCount == 0)) {
        _stats.crcErrors++;
    }

    (void)extractedCount;
    _rxBuffer.clear();
    return;
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
    if (length < 4) return false;
    
    frame.unitId = data[0];
    frame.functionCode = data[1];
    frame.timestamp = millis();
    frame.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
    frame.isRequest = false;
    
    // Verify CRC (Modbus: LSB first)
    uint16_t receivedCrc = (uint16_t)data[length - 2] | ((uint16_t)data[length - 1] << 8);
    uint16_t calculatedCrc = calculateCRC(data, length - 2);

    const size_t payloadLenRaw = length - 4; // exclude unit, fc, crc(2)
    const size_t payloadLen = (payloadLenRaw <= ModbusFrame::MAX_DATA_LEN) ? payloadLenRaw : ModbusFrame::MAX_DATA_LEN;
    
    if (receivedCrc != calculatedCrc) {
        frame.isValid = false;
        frame.crc = receivedCrc;
        frame.dataLen = (uint16_t)payloadLen;
        if (payloadLen > 0) memcpy(frame.data.data(), data + 2, payloadLen);
        frame.isException = false;
        frame.exceptionCode = 0;
        return true;  // Return true so caller can count CRC error and log
    }
    
    frame.crc = receivedCrc;
    frame.isValid = true;
    
    // Check for exception
    if (frame.functionCode & 0x80) {
        frame.isException = true;
        frame.exceptionCode = (length > 2) ? data[2] : 0;
        frame.dataLen = 0;
    } else {
        frame.isException = false;
        frame.exceptionCode = 0;
        frame.dataLen = (uint16_t)payloadLen;
        if (payloadLen > 0) memcpy(frame.data.data(), data + 2, payloadLen);
    }
    
    return true;
}

void ModbusRTUFeature::updateRegisterMap(const ModbusFrame& request, const ModbusFrame& response) {
    if (!response.isValid || response.isException) return;
    
    uint8_t fc = response.functionCode;
    if (fc != ModbusFC::READ_HOLDING_REGISTERS && 
        fc != ModbusFC::READ_INPUT_REGISTERS &&
        fc != ModbusFC::READ_COILS &&
        fc != ModbusFC::READ_DISCRETE_INPUTS) {
        return;  // Not a read response
    }
    
    ModbusRegisterMap& regMap = ensureRegisterMap(response.unitId, fc);
    regMap.responseCount++;
    regMap.lastUpdate = millis();
    
    // Extract register values from response
    uint16_t startReg = request.getStartRegister();
    size_t byteCount = response.getByteCount();
    const uint8_t* regData = response.getRegisterData();
    
    if (!regData || byteCount == 0) return;
    
    if (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS) {
        // Each register is 2 bytes
        size_t regCount = byteCount / 2;
        for (size_t i = 0; i < regCount; i++) {
            uint16_t value = (regData[i * 2] << 8) | regData[i * 2 + 1];
            regMap.registers[startReg + i] = value;
        }
    } else {
        // Coils/discrete inputs: 1 bit per coil, packed into bytes
        for (size_t i = 0; i < byteCount * 8; i++) {
            uint16_t value = (regData[i / 8] >> (i % 8)) & 0x01;
            regMap.registers[startReg + i] = value;
        }
    }
}

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
        _gapSchedulerStats.predictionsUsed++;
    }

    // Pick the first request whose unit isn't paused.
    size_t sendIndex = (size_t)-1;
    for (size_t i = 0; i < _requestQueue.size(); i++) {
        if (!isUnitQueueingPaused(_requestQueue[i].unitId)) {
            sendIndex = i;
            break;
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
    // Require MIN_GAP_SILENCE_MS of bus silence before sending.  This ensures
    // we're past all inter-request gaps in the foreign master's cycle (~10-160ms)
    // and into the wrap-around gap (~243-1759ms).  After our own TX+response,
    // _lastByteTime resets so we naturally wait another 200ms before re-sending,
    // allowing multiple TXes per wrap-around gap when it's long enough.
    //
    // Additional safety: check UART RX buffer immediately before TX to catch
    // foreign frames that arrived between the silence measurement and now.
    static constexpr uint32_t MIN_GAP_SILENCE_MS = 200;

    // Don't TX until we've observed at least one foreign transaction.
    // At boot, _lastByteTime is from initialization and doesn't reflect
    // actual bus activity — the foreign master may be mid-cycle.
    if (!_hasLastTransactionEnd) return;

    uint32_t actualSilenceMs = (uint32_t)((micros() - _lastByteTime) / 1000ULL);

    {
        uint32_t wireMs = estimateWireTimeMs(_requestQueue[sendIndex].quantity);

        if (actualSilenceMs < MIN_GAP_SILENCE_MS) {
            _gapSchedulerStats.txDeferred++;
            return;
        }

        // Last-resort UART check: if bytes arrived between the silence
        // measurement above and now, a foreign frame is starting
        if (_serial.available() > 0) {
            _gapSchedulerStats.txDeferred++;
            LOG_V("TX deferred: UART RX bytes pending at decision point");
            return;
        }

        // Log TX-time context for post-mortem collision analysis
        uint32_t txElapsed = _gapWindowActive ? (uint32_t)(millis() - _gapWindowOpenMs) : 0;
        _lastTxElapsedMs = txElapsed;
        _lastTxWireMs = wireMs;
        _sentDuringGapWindow = gap.valid;
        LOG_I("Gap TX: silence=%ums wire=%ums qty=%u unit=%u gap=%s",
              actualSilenceMs, wireMs, _requestQueue[sendIndex].quantity,
              _requestQueue[sendIndex].unitId, gap.valid ? "yes" : "fallback");
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

        // Don't close the gap window after our TX: _lastByteTime is updated
        // by the response bytes, so the 200ms silence check naturally prevents
        // re-sending too quickly.  Keeping the window open allows multiple
        // TXes per wrap-around gap when it's long enough.

        // Gap scheduler stats
        uint32_t wireMs = estimateWireTimeMs(req.quantity);
        _gapSchedulerStats.totalWireTimeMs += wireMs;
        _gapSchedulerStats.lastTxMs = millis();
        _sentDuringGapWindow = gap.valid;
        if (gap.valid) {
            _gapSchedulerStats.txInGap++;
            _gapSchedulerStats.predictionsUsed++;
        } else {
            _gapSchedulerStats.txFallback++;
        }

        // Track our own FC3/FC4 requests in the register map
        uint8_t fc = req.functionCode & 0x7F;
        if (fc == ModbusFC::READ_HOLDING_REGISTERS || fc == ModbusFC::READ_INPUT_REGISTERS) {
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
    // Last-moment safety check: if bytes are already pending in the UART RX
    // buffer, a foreign frame is arriving (or just arrived).  Abort TX to
    // avoid colliding on the bus.
    int pending = _serial.available();
    if (pending > 0) {
        LOG_W("TX aborted: %d bytes pending in UART RX — foreign frame arriving", pending);
        _gapSchedulerStats.txDeferred++;
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

    // Mark end-of-TX as last bus activity for accurate silence detection.
    _lastByteTime = micros();

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
    char hexBuf[64];
    size_t hexLen = 0;
    size_t totalLen = _txFrameLen + 2; // +2 for CRC
    for (size_t i = 0; i < _txFrameLen && hexLen < 58; i++) {
        hexLen += snprintf(hexBuf + hexLen, sizeof(hexBuf) - hexLen, "%02X ", _txFrameBuffer[i]);
    }
    hexLen += snprintf(hexBuf + hexLen, sizeof(hexBuf) - hexLen, "%02X %02X", crc & 0xFF, crc >> 8);
    LOG_I("TX[%u]: %s", totalLen, hexBuf);
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
    uint16_t crc = 0xFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return crc;
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
    String result;
    result.reserve(length * 3);  // "XX " per byte, avoids per-byte reallocation
    for (size_t i = 0; i < length; i++) {
        if (i > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }
    return result;
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
    if (!frame.isRequest || !frame.isValid) return;

    uint8_t fc = frame.functionCode & 0x7F;
    // Only track read requests (FC3/FC4) — they have standard 4-byte payload
    if (fc != ModbusFC::READ_HOLDING_REGISTERS && fc != ModbusFC::READ_INPUT_REGISTERS) return;
    if (frame.dataLen < 4) return;

    uint16_t startReg = frame.getStartRegister();
    uint16_t qty      = frame.getQuantity();
    uint64_t key      = makeBusPatternKey(frame.unitId, fc, startReg, qty);

    auto it = _busPatterns.find(key);
    if (it == _busPatterns.end()) {
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

    // Record successor gap on the PREVIOUS completed transaction
    // "After transaction X finished, the bus was idle for Y ms before this request"
    //
    // Filter: discard gaps shorter than the Modbus RTU inter-frame minimum (3.5 char times).
    // When the ESP32 loop() is slower than the bus, multiple frames coalesce in the same
    // RX buffer.  The frame timestamps are derived from byte-index × charTime, which treats
    // coalesced frames as contiguous and collapses the inter-frame silence to ~0ms.
    // These sub-minimum "gaps" are measurement artefacts, not real bus silences.
    // Use 10ms floor: at 9600 baud an 8-byte request takes ~8ms, so any real gap
    // must account for at least the inter-frame silence plus physical turnaround.
    const uint32_t minInterframeCalc = (uint32_t)(3.5 * _charTimeUs / 1000.0) + 1;
    const uint32_t minInterframeMs = (minInterframeCalc < 10) ? 10 : minInterframeCalc;
    if (_hasLastCompletedTx && _hasLastTransactionEnd && frame.timestamp > _lastTransactionEndMs) {
        uint32_t gapMs = (uint32_t)(frame.timestamp - _lastTransactionEndMs);
        if (gapMs >= minInterframeMs) {
            auto predIt = _busPatterns.find(_lastCompletedTxKey);
            if (predIt != _busPatterns.end()) {
                predIt->second.recordSuccessorGap(gapMs);
            }
            // Record transition with gap: predecessor -> this request
            _busTransitions[_lastCompletedTxKey][key].record(gapMs);

            // Track global minimum gap across ALL transitions.
            // Used as a floor in canSafelyTransmitInGap to protect against
            // unobserved successor edges.
            if (gapMs < _globalMinGapMs) {
                _globalMinGapMs = gapMs;
            }
        }
    }

    // Record into cycle sequence ring buffer
    _cycleSeq[_cycleSeqIndex] = key;
    _cycleSeqIndex = (_cycleSeqIndex + 1) % CYCLE_SEQ_SIZE;
    _cycleSeqCount++;

    // Cycle position tracking for gap prediction
    if (!_detectedCycle.empty()) {
        if (_cycleTrackingPos < 0) {
            // Try to sync: find this request in the cycle
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
                // Match! Record gap since last transaction ended
                if (_hasLastTransactionEnd && frame.timestamp > _lastTransactionEndMs) {
                    uint32_t gapMs = (uint32_t)(frame.timestamp - _lastTransactionEndMs);
                    if (gapMs >= minInterframeMs && pos < _cycleStepGaps.size()) {
                        _cycleStepGaps[pos].record(gapMs);
                    }
                }
                _cycleTrackingPos = (int)((pos + 1) % _detectedCycle.size());
            } else {
                // Mismatch — try to find in cycle and resync
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
                if (!found) _cycleTrackingPos = -1;  // Lost sync
            }
        }
    }
}

void ModbusRTUFeature::recordBusGap(uint32_t gapUs) {
    _busGapStats.record(gapUs);
}

// ---- Gap-aware TX scheduling ----

GapPrediction ModbusRTUFeature::predictCurrentGap() const {
    GapPrediction result;

    if (!_hasLastCompletedTx) return result;

    auto predIt = _busTransitions.find(_lastCompletedTxKey);
    if (predIt == _busTransitions.end()) return result;

    const auto& successors = predIt->second;
    if (successors.empty()) return result;

    // Sum up total observations across all successors (only edges with ≥2 samples)
    uint32_t totalSamples = 0;
    uint32_t usableEdges = 0;
    for (const auto& kv : successors) {
        if (kv.second.count >= 2) {
            totalSamples += kv.second.count;
            usableEdges++;
        }
    }

    if (totalSamples < GAP_MIN_SAMPLES || usableEdges == 0) return result;

    // Most-likely successor prediction with gap confirmation.
    //
    // Two-phase approach for bimodal gap distributions:
    //   1. Compute confirmationMs = min conservative across ALL successors.
    //      Wait at least this long before transmitting.  If no foreign request
    //      arrives within confirmationMs, the short-gap successors are ruled out.
    //   2. Use the most-likely successor's conservative gap for the total
    //      predicted gap (optimistic but safe after confirmation).
    //
    // This avoids both extremes: min-across-all (~10ms, too conservative) and
    // most-likely-only (~300ms, causes collisions when rare successors occur).
    const BusTransitionEntry* bestEdge = nullptr;
    uint32_t bestCount = 0;
    uint32_t hardMinObserved = UINT32_MAX;
    uint32_t minConservativeAll = UINT32_MAX;  // confirmation threshold

    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        if (te.count < 2) continue;  // need at least 2 samples for stddev

        double mean = te.gapSum / te.count;
        double variance = (te.gapSumSq / te.count) - (mean * mean);
        double stddev = (variance > 0) ? sqrt(variance) : 0;

        // Conservative per-edge: mean - 1*stddev, floored at observed minimum
        double conservative = mean - stddev;
        if (conservative < (double)te.gapMin) conservative = (double)te.gapMin;
        if (conservative < 0) conservative = 0;
        uint32_t conservativeMs = (uint32_t)conservative;

        if (conservativeMs < minConservativeAll) {
            minConservativeAll = conservativeMs;
        }

        if (te.count > bestCount) {
            bestCount = te.count;
            bestEdge = &te;
        }
        if (te.gapMin < hardMinObserved) {
            hardMinObserved = te.gapMin;
        }
    }

    if (!bestEdge) return result;

    double mean = bestEdge->gapSum / bestEdge->count;
    double variance = (bestEdge->gapSumSq / bestEdge->count) - (mean * mean);
    double stddev = (variance > 0) ? sqrt(variance) : 0;

    // Conservative estimate for the most-likely (dominant) successor
    double bestConservative = mean - stddev;
    if (bestConservative < (double)bestEdge->gapMin) bestConservative = (double)bestEdge->gapMin;
    if (bestConservative < 0) bestConservative = 0;

    // Apply safety margin to the predicted gap
    uint32_t predicted = (uint32_t)(bestConservative * (1.0 - (double)_gapSchedulerStats.safetyMargin));

    // Confirmation threshold: wait at least this long to rule out short-gap
    // successors.  Apply a small buffer (+2ms) to account for timing jitter.
    uint32_t confirmation = (minConservativeAll != UINT32_MAX) ? (minConservativeAll + 2) : 0;

    result.valid = true;
    result.predictedGapMs = predicted;
    result.confirmationMs = confirmation;
    result.minObservedMs = hardMinObserved;
    result.sampleCount = totalSamples;
    return result;
}

bool ModbusRTUFeature::canSafelyTransmitInGap(uint32_t wireMs) const {
    if (!_hasLastCompletedTx || !_gapWindowActive) return false;

    auto predIt = _busTransitions.find(_lastCompletedTxKey);
    if (predIt == _busTransitions.end()) return false;

    const auto& successors = predIt->second;
    if (successors.empty()) return false;

    uint32_t elapsedMs = (uint32_t)(millis() - _gapWindowOpenMs);

    // Global minimum gap floor: protect against UNOBSERVED successors.
    // If ANY transition in the entire map has a gap as short as X ms,
    // assume any predecessor could have a successor that fast.
    // Don't transmit until elapsed exceeds the global minimum, ensuring
    // that even an unseen fast-chain successor would have arrived by now.
    if (_globalMinGapMs != UINT32_MAX && elapsedMs < _globalMinGapMs) {
        return false;
    }

    // Fixed safety buffer for timing jitter (loop delay, processing, etc.)
    static constexpr uint32_t FIXED_BUFFER_MS = 15;
    uint32_t safeWireMs = wireMs + FIXED_BUFFER_MS;

    // Check ALL successor edges (including count=1).
    // Use gapMin for ruling out and for fit checking, but apply a
    // count-based safety haircut: with few observations, the observed
    // gapMin might not be the true minimum.  Shrink it proportionally:
    //   effectiveMin = gapMin * count / (count + K)
    // K=5: count=3 → 37.5%, count=10 → 66.7%, count=100 → 95.2%
    static constexpr uint32_t CONFIDENCE_K = 5;

    bool hasAnyEdge = false;
    bool atLeastOneNotRuledOut = false;
    for (const auto& kv : successors) {
        const BusTransitionEntry& te = kv.second;
        hasAnyEdge = true;

        // Apply confidence haircut to gapMin for low-count edges.
        uint32_t effectiveMin = (uint32_t)((uint64_t)te.gapMin * te.count / (te.count + CONFIDENCE_K));

        // Rule out: elapsed exceeds gapMin — the successor would have
        // already sent its request.  (Use raw gapMin for rule-out, since
        // underestimating gapMin here is SAFE — it keeps edges in play longer.)
        if (elapsedMs >= te.gapMin) continue;

        atLeastOneNotRuledOut = true;

        // Fit check: TX must complete before the effective minimum gap.
        if (elapsedMs + safeWireMs > effectiveMin) {
            return false;  // This successor could collide with our TX
        }
    }

    // If ALL successors were ruled out, something is wrong — we should have
    // seen a foreign request by now, but didn't (e.g., corrupted by a previous
    // collision).  Don't transmit blindly; the gap state is likely stale.
    if (!atLeastOneNotRuledOut) return false;

    return hasAnyEdge;
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

void ModbusRTUFeature::reportCollision() {
    _gapSchedulerStats.collisions++;
    _gapSchedulerStats.gapInsufficient++;
    _gapSchedulerStats.lastCollisionMs = millis();

    // Log detailed context about the collision for diagnostics
    LOG_W("COLLISION: sentInGap=%s txElapsed=%ums txWire=%ums globalMin=%ums",
          _sentDuringGapWindow ? "yes" : "no", _lastTxElapsedMs, _lastTxWireMs,
          _globalMinGapMs != UINT32_MAX ? _globalMinGapMs : 0);

    if (_hasLastCompletedTx) {
        auto predIt = _busTransitions.find(_lastCompletedTxKey);
        if (predIt != _busTransitions.end()) {
            for (const auto& kv : predIt->second) {
                const BusTransitionEntry& te = kv.second;
                LOG_W("  successor gapMin=%u count=%u mean=%.0f",
                      te.gapMin, te.count, te.count > 0 ? te.gapSum / te.count : 0.0);
            }
        }
    }

    // Increase safety margin by 5% on each collision, up to max
    float oldMargin = _gapSchedulerStats.safetyMargin;
    _gapSchedulerStats.safetyMargin = std::min(
        _gapSchedulerStats.safetyMargin + 0.05f,
        _gapSchedulerStats.maxMargin
    );
    if (_gapSchedulerStats.safetyMargin != oldMargin) {
        _gapSchedulerStats.lastMarginAdjustMs = millis();
        LOG_W("Gap scheduler: margin %.0f%% -> %.0f%%",
              oldMargin * 100, _gapSchedulerStats.safetyMargin * 100);
    }
}

void ModbusRTUFeature::resetBusPatterns() {
    _busPatterns.clear();
    _busGapStats.reset();
    _busByteStats.reset();
    _busTransactionStats.reset();
    _hasLastFrameBoundary = false;
    _lastFrameBoundaryUs = 0;
    _detectedCycle.clear();
    _cycleStepGaps.clear();
    _cycleTrackingPos = -1;
    _lastTransactionEndMs = 0;
    _hasLastTransactionEnd = false;
    _lastCompletedTxKey = 0;
    _hasLastCompletedTx = false;
    _globalMinGapMs = UINT32_MAX;
    _busTransitions.clear();
    _gapSchedulerStats.reset();
    _gapWindowActive = false;
    _gapWindowBudgetMs = 0;
    _gapWindowUsedMs = 0;
    _sentDuringGapWindow = false;
    _cycleSeqIndex = 0;
    _cycleSeqCount = 0;
    memset(_cycleSeq, 0, sizeof(_cycleSeq));
    LOG_I("Bus pattern tracking reset");
}

void ModbusRTUFeature::detectCycle() {
    _detectedCycle.clear();

    // Need at least some data
    size_t available = (_cycleSeqCount < CYCLE_SEQ_SIZE) ? _cycleSeqCount : CYCLE_SEQ_SIZE;
    if (available < 4) return;

    // Build the sequence in chronological order (stack array, avoids heap allocation)
    uint64_t seq[CYCLE_SEQ_SIZE];
    if (_cycleSeqCount >= CYCLE_SEQ_SIZE) {
        // Ring buffer wrapped — oldest is at _cycleSeqIndex
        for (size_t i = 0; i < CYCLE_SEQ_SIZE; ++i) {
            seq[i] = _cycleSeq[(_cycleSeqIndex + i) % CYCLE_SEQ_SIZE];
        }
    } else {
        for (size_t i = 0; i < _cycleSeqCount; ++i) {
            seq[i] = _cycleSeq[i];
        }
    }

    // Try cycle lengths from 1 to available/2
    size_t bestLen = 0;
    size_t bestMatches = 0;
    for (size_t tryLen = 1; tryLen <= available / 2 && tryLen <= 64; ++tryLen) {
        size_t matches = 0;
        for (size_t i = tryLen; i < available; ++i) {
            if (seq[i] == seq[i % tryLen]) {
                matches++;
            }
        }
        // Require >80% match rate for a valid cycle
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

        // Allocate per-step gap tracking and reset tracking position
        _cycleStepGaps.assign(_detectedCycle.size(), CycleStepStats{});
        _cycleTrackingPos = -1;  // Will sync on next matching request
    }
}
