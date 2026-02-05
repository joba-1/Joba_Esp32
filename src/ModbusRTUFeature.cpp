#include "ModbusRTUFeature.h"
#include <esp_heap_caps.h>
#include <algorithm>
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
        _frameHistory[i].data.clear();
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
    result.reserve(3 * (2 + frame.data.size() + 2));

    auto appendByte = [&](uint8_t b) {
        if (result.length() > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", b);
        result += buf;
    };

    appendByte(frame.unitId);
    appendByte(frame.functionCode);
    for (uint8_t b : frame.data) appendByte(b);
    appendByte((uint8_t)(frame.crc & 0xFF));
    appendByte((uint8_t)((frame.crc >> 8) & 0xFF));
    return result;
}

uint16_t ModbusRTUFeature::calculateFrameCrc(const ModbusFrame& frame) const {
    // Reconstruct the bytes that CRC is computed over: unit + fc + payload.
    // For exception frames, the payload is the single exception code byte.
    std::vector<uint8_t> bytes;
    bytes.reserve(2 + (frame.isException ? 1 : frame.data.size()));
    bytes.push_back(frame.unitId);
    bytes.push_back(frame.functionCode);
    if (frame.isException) {
        bytes.push_back(frame.exceptionCode);
    } else {
        bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());
    }
    return calculateCRC(bytes.data(), bytes.size());
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
    
    _ready = true;
}

void ModbusRTUFeature::loop() {
    if (!_ready) return;

    _loopCounter++;
    
    unsigned long nowUs = micros();
    unsigned long nowMs = millis();
    
    // Track total time for statistics
    _stats.totalTimeUs += (nowUs - _activeStartTimeUs);
    if (!_inActiveTime) {
        _activeStartTimeUs = nowUs;
    }
    
    // Read available data, using per-byte timestamps to detect inter-character gaps.
    // IMPORTANT: Do not drain indefinitely. On a busy bus the UART can remain non-empty
    // continuously, which would starve the rest of the firmware and prevent TX arbitration.
    // Also: when we have queued requests to send, favor draining RX so we can find
    // an inter-frame gap and transmit.
    const bool wantsToTransmitSoon = (!_waitingForResponse && !_requestQueue.empty());
    const size_t maxRxBytesThisLoop = wantsToTransmitSoon ? 1024 : 256;
    size_t rxBytesThisLoop = 0;
    while (_serial.available() && rxBytesThisLoop < maxRxBytesThisLoop) {
        unsigned long byteTimeUs = micros();
        uint8_t byte = _serial.read();
        rxBytesThisLoop++;

        // Check for inter-character timeout (1.5 char times = new frame start)
        if (_rxBuffer.size() > 0 && (byteTimeUs - _lastByteTime) > (_charTimeUs * 15 / 10)) {
            // Process previous frame before starting new one
            processReceivedData();
        }

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
        
        // DO NOT invoke callback on timeout - callbacks can block and trigger watchdog
        // The timeout is already logged, which provides visibility
        
        _waitingForResponse = false;
        _hasPendingRequest = false;
        endActiveTime();
        
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
        const bool gapEnoughForTx = _serialWasEmpty && (idleUs > requiredIdleUs);

        _dbgGapUsInLoop = idleUs;
        _dbgGapEnoughForTxInLoop = gapEnoughForTx;
        _dbgLastLoopSnapshotMs = millis();

        if (gapEnoughForTx) {
            processQueue();
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

                    // Frame boundary detection while arbitrating.
                    if (_rxBuffer.size() > 0 && (byteTimeUs - _lastByteTime) > (_charTimeUs * 15 / 10)) {
                        processReceivedData();
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
                    processQueue();
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
        LOG_D("Incomplete frame received (size=%u)", _rxBuffer.size());
        _rxBuffer.clear();
        return;
    }

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
                if (r != TryParseResult::Fail && !frame.isException && frame.data.size() == 4) {
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
                                if (req.isValid && reqFc == fc && req.data.size() == 4) {
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
            _stats.crcErrors++;
            LOG_D("RX Frame (CRC ERROR): Unit=%d, FC=0x%02X, Raw=%s",
                frame.unitId, frame.functionCode,
                formatFrameHex(frame).c_str());
            recordFrameToHistory(frame);
            if (_frameCallback) {
                _frameCallback(frame, isRequest);
            }
            i += frameLen;
            continue;
          }

          // CRC-valid frame
          _stats.framesReceived++;

          LOG_D("RX Frame: Unit=%d, FC=0x%02X, Data=%s, CRC=0x%04X",
              frame.unitId, frame.functionCode,
              formatHex(frame.data.data(), frame.data.size()).c_str(),
              frame.crc);

          // Debug: store in frame history (also closes pending CRC contexts)
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

            // Reset backoff for this unit only
            _backoffByUnit.erase(frame.unitId);
            _lastSuccessTime = millis();

            if (!frame.isException) {
                _stats.ownRequestsSuccess++;
                _intervalStats.ownSuccess++;
            } else {
                _stats.ownRequestsFailed++;
                _intervalStats.ownFailed++;
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
                startActiveTime(false);
                }
            } else {
                if (frame.isException) {
                    _stats.otherExceptionsSeen++;
                    _intervalStats.otherFailed++;

                    // Pairing quality (best-effort): try to associate exception with a recent request
                    // from the same unit and matching FC.
                    {
                        uint8_t exFc = frame.functionCode & 0x7F;
                        bool paired = false;
                        if (exFc == FC3 || exFc == FC4) {
                            auto reqIt = _lastRequestPerUnit.find(frame.unitId);
                            if (reqIt != _lastRequestPerUnit.end()) {
                                const ModbusFrame& req = reqIt->second;
                                if (req.isValid && ((req.functionCode & 0x7F) == exFc) && req.data.size() == 4) {
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
                }

                if (!frame.isException) {
                    auto reqIt = _lastRequestPerUnit.find(frame.unitId);
                    bool updated = false;
                    if (reqIt != _lastRequestPerUnit.end()) {
                        const ModbusFrame& req = reqIt->second;
                        if (req.isValid && ((req.functionCode & 0x7F) == (frame.functionCode & 0x7F)) && req.data.size() == 4) {
                            if ((frame.timestamp - req.timestamp) < 2000) {
                                updateRegisterMap(req, frame);
                                updated = true;
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
    
    if (receivedCrc != calculatedCrc) {
        frame.isValid = false;
        frame.crc = receivedCrc;
        frame.data.assign(data + 2, data + length - 2);
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
        frame.data.clear();
    } else {
        frame.isException = false;
        frame.exceptionCode = 0;
        frame.data.assign(data + 2, data + length - 2);
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

void ModbusRTUFeature::processQueue() {
    if (_requestQueue.empty()) return;

    // Pick the first request whose unit isn't paused.
    size_t sendIndex = (size_t)-1;
    for (size_t i = 0; i < _requestQueue.size(); i++) {
        if (!isUnitQueueingPaused(_requestQueue[i].unitId)) {
            sendIndex = i;
            break;
        }
    }
    if (sendIndex == (size_t)-1) return;

    _processQueueCounter++;
    _lastProcessQueueMs = millis();
    
    // Copy the selected request (not reference/pointer - prevents vector reallocation issues)
    ModbusPendingRequest req = _requestQueue[sendIndex];
    LOG_D("Processing request: Unit=%d, FC=0x%02X, Addr=%d, Qty=%d",
          req.unitId, req.functionCode, req.startRegister, req.quantity);
    
    if (sendRequest(req)) {
        LOG_D("Request sent successfully");
        _stats.ownRequestsSent++;

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
        _lastRequest.data.clear();
        _lastRequest.data.push_back(req.startRegister >> 8);
        _lastRequest.data.push_back(req.startRegister & 0xFF);
        _lastRequest.data.push_back(req.quantity >> 8);
        _lastRequest.data.push_back(req.quantity & 0xFF);

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
    std::vector<uint8_t> frame;
    
    frame.push_back(request.unitId);
    frame.push_back(request.functionCode);
    
    switch (request.functionCode) {
        case ModbusFC::READ_COILS:
        case ModbusFC::READ_DISCRETE_INPUTS:
        case ModbusFC::READ_HOLDING_REGISTERS:
        case ModbusFC::READ_INPUT_REGISTERS:
            frame.push_back(request.startRegister >> 8);
            frame.push_back(request.startRegister & 0xFF);
            frame.push_back(request.quantity >> 8);
            frame.push_back(request.quantity & 0xFF);
            break;
            
        case ModbusFC::WRITE_SINGLE_REGISTER:
            frame.push_back(request.startRegister >> 8);
            frame.push_back(request.startRegister & 0xFF);
            if (!request.writeData.empty()) {
                frame.push_back(request.writeData[0] >> 8);
                frame.push_back(request.writeData[0] & 0xFF);
            }
            break;
            
        case ModbusFC::WRITE_MULTIPLE_REGISTERS:
            frame.push_back(request.startRegister >> 8);
            frame.push_back(request.startRegister & 0xFF);
            frame.push_back(request.quantity >> 8);
            frame.push_back(request.quantity & 0xFF);
            frame.push_back(request.quantity * 2);  // Byte count
            for (uint16_t val : request.writeData) {
                frame.push_back(val >> 8);
                frame.push_back(val & 0xFF);
            }
            break;
            
        default:
            LOG_E("Unsupported function code: 0x%02X", request.functionCode);
            return false;
    }
    
    sendFrame(frame);
    return true;
}

void ModbusRTUFeature::sendFrame(const std::vector<uint8_t>& frame) {
    // Calculate and append CRC
    uint16_t crc = calculateCRC(frame.data(), frame.size());
    
    setDE(true);  // Enable transmitter
    delayMicroseconds(100);  // Small delay for transceiver
    
    for (uint8_t byte : frame) {
        _serial.write(byte);
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
    
    LOG_V("Modbus TX: unit=%u, FC=0x%02X, len=%u", frame[0], frame[1], frame.size());
}

bool ModbusRTUFeature::sendRawFrame(const uint8_t* data, size_t length) {
    if (!_busSilent) return false;
    
    std::vector<uint8_t> frame(data, data + length);
    sendFrame(frame);
    return true;
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
        LOG_E("Modbus request DISCARDED: critical heap (%u bytes) - unit %d FC 0x%02X",
              freeHeap, unitId, functionCode);
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
        LOG_E("Modbus write request DISCARDED: critical heap (%u bytes) - unit %d reg %d",
              freeHeap, unitId, address);
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
        LOG_E("Modbus write-multi request DISCARDED: critical heap (%u bytes) - unit %d",
              freeHeap, unitId);
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
        LOG_I("Modbus stats (%lus): own=%u/%u ok, other=%u req, CRC=%u, idle=%.1f%%",
              uptimeSec, 
              _stats.ownRequestsSuccess, totalOwn,
              _stats.otherRequestsSeen,
              _stats.crcErrors,
              getBusIdlePercent());
    }
    
    // Reset interval stats for next period
    resetIntervalStats();
}

String ModbusRTUFeature::formatHex(const uint8_t* data, size_t length) const {
    String result;
    for (size_t i = 0; i < length; i++) {
        if (i > 0) result += ' ';
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        result += buf;
    }
    return result;
}
