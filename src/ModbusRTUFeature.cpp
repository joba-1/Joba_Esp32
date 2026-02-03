#include "ModbusRTUFeature.h"
#include <esp_heap_caps.h>
#include "TimeUtils.h"

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
    , _consecutiveTimeouts(0)
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
    
    unsigned long nowUs = micros();
    unsigned long nowMs = millis();
    
    // Track total time for statistics
    _stats.totalTimeUs += (nowUs - _activeStartTimeUs);
    if (!_inActiveTime) {
        _activeStartTimeUs = nowUs;
    }
    
    // Read available data, using per-byte timestamps to detect inter-character gaps.
    while (_serial.available()) {
        unsigned long byteTimeUs = micros();
        uint8_t byte = _serial.read();

        // Check for inter-character timeout (1.5 char times = new frame start)
        if (_rxBuffer.size() > 0 && (byteTimeUs - _lastByteTime) > (_charTimeUs * 15 / 10)) {
            // Process previous frame before starting new one
            processReceivedData();
        }

        _rxBuffer.push_back(byte);
        _lastByteTime = byteTimeUs;
        _lastActivityTime = nowMs;
        _busSilent = false;

        // Start tracking active time if not already
        if (!_inActiveTime && !_waitingForResponse) {
            startActiveTime(false);  // Assume other device traffic
        }
    }

    // Re-evaluate current time before checking for frame-complete silence
    nowUs = micros();

    // Check for frame complete (3.5 char silence)
    if (_rxBuffer.size() > 0 && (nowUs - _lastByteTime) > _silenceTimeUs) {
        processReceivedData();
    }
    
    // Update bus silence state and end active time tracking
    if (!_busSilent && (nowMs - _lastActivityTime) > (_silenceTimeUs / 1000 + 1)) {
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
        
        // Track consecutive timeouts to trigger backoff
        _consecutiveTimeouts++;
        if (_consecutiveTimeouts == 3) {
            LOG_W("Modbus: 3 consecutive timeouts detected, pausing queue to prevent memory exhaustion");
        }
        
        // DO NOT invoke callback on timeout - callbacks can block and trigger watchdog
        // The timeout is already logged, which provides visibility
        
        _waitingForResponse = false;
        _hasPendingRequest = false;
        endActiveTime();
        
        // Aggressive queue clearing on repeated timeouts to prevent memory exhaustion
        // If queue has accumulated many items, this indicates slave is unresponsive
        if (_requestQueue.size() > _maxQueueSize / 2) {
            LOG_W("Modbus queue building up (%u items), clearing to prevent memory exhaustion",
                  _requestQueue.size());
            _requestQueue.clear();
        }
    }
    
    // Process request queue when bus is silent and not waiting
    if (_busSilent && !_waitingForResponse) {
        processQueue();
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

        auto tryParseAtLen = [&](size_t len) -> bool {
            if (remaining < len) return false;
            if (!parseFrame(p, len, frame)) return false;
            return frame.isValid;
        };

        // Exceptions for FC3/FC4 are fixed length: unit + fc|0x80 + excCode + crc(2) = 5
        if ((fc == FC3_EX || fc == FC4_EX) && remaining >= 5) {
            if (tryParseAtLen(5) && frame.isException) {
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
                if (tryParseAtLen(8) && !frame.isException && frame.data.size() == 4) {
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
                    if (tryParseAtLen(respLen) && !frame.isException) {
                        // Optional stronger validation using last seen request for this unit/fc
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

        // At this point we have a spec-valid, CRC-valid frame.
        extractedCount++;
        _stats.framesReceived++;

        frame.isRequest = isRequest;

        LOG_D("RX Frame: Unit=%d, FC=0x%02X, Data=%s, CRC=0x%04X",
              frame.unitId, frame.functionCode,
              formatHex(frame.data.data(), frame.data.size()).c_str(),
              frame.crc);

        // Debug: store in frame history
        _frameHistory[_frameHistoryIndex] = frame;
        _frameHistoryIndex = (_frameHistoryIndex + 1) % FRAME_HISTORY_SIZE;

        bool isOurResponse = false;

        // Match our response more strictly: unitId + functionCode (or exception variant)
        uint8_t expectedFc = _currentRequest.functionCode;
        bool fcMatches = (frame.functionCode == expectedFc) ||
                         (frame.isException && ((frame.functionCode & 0x7F) == (expectedFc & 0x7F)));

        if (_waitingForResponse && _hasPendingRequest && frame.isValid &&
            frame.unitId == _currentRequest.unitId && fcMatches) {
            isRequest = false;
            isOurResponse = true;
            _waitingForResponse = false;

            _consecutiveTimeouts = 0;
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
                // FC3/FC4: track per-unit requests and register-map requestCount
                uint8_t reqFc = frame.functionCode & 0x7F;
                if (reqFc == FC3 || reqFc == FC4) {
                    ModbusRegisterMap& map = ensureRegisterMap(frame.unitId, reqFc);
                    map.requestCount++;
                    map.lastUpdate = millis();
                }

                _lastRequestPerUnit[frame.unitId] = frame;
                _lastRequest = frame;
                _stats.otherRequestsSeen++;
                startActiveTime(false);
            } else {
                if (frame.isException) {
                    _stats.otherExceptionsSeen++;
                    _intervalStats.otherFailed++;

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
                }
            }
        }

        if (_frameCallback) {
            _frameCallback(frame, isRequest);
        }

        i += frameLen;
    }

    // If there were leftover bytes that didn't form any valid frame, count as CRC/noise once.
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
    
    // Copy the front request (not reference/pointer - prevents vector reallocation issues)
    ModbusPendingRequest req = _requestQueue.front();
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
    } else {
        LOG_W("Failed to send request - bus not silent");
    }
    
    _requestQueue.erase(_requestQueue.begin());
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
    
    // Discard any RX echo of our transmission (half-duplex RS485 can read own bytes back).
    // Otherwise we either treat our echo as the response or concatenate echo+response and get CRC errors.
    delayMicroseconds(_charTimeUs * 2);  // Allow echo to appear on line
    int discarded = 0;
    while (_serial.available()) {
        (void)_serial.read();
        discarded++;
    }
    if (discarded > 0) {
        LOG_V("Modbus TX: discarded %d RX echo bytes", discarded);
    }
    _rxBuffer.clear();
    
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
    // If we're experiencing repeated timeouts, stop queueing to prevent memory exhaustion
    // Allow one retry after timeout before giving up
    if (_consecutiveTimeouts > 2) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        
        // Throttle warning message - only log once per 60 seconds to avoid spam
        unsigned long nowMs = millis();
        if ((nowMs - _lastTimeoutWarningMs) >= 60000) {
            LOG_W("Modbus: Queueing paused - %u consecutive timeouts (discarding requests)",
                  _consecutiveTimeouts);
            _lastTimeoutWarningMs = nowMs;
        }
        return false;
    }
    
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
    // If we're experiencing repeated timeouts, stop queueing
    if (_consecutiveTimeouts > 2) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        
        // Throttle warning message - only log once per 60 seconds
        unsigned long nowMs = millis();
        if ((nowMs - _lastTimeoutWarningMs) >= 60000) {
            LOG_W("Modbus: Queueing paused - %u consecutive timeouts (discarding requests)",
                  _consecutiveTimeouts);
            _lastTimeoutWarningMs = nowMs;
        }
        return false;
    }
    
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
    // If we're experiencing repeated timeouts, stop queueing
    if (_consecutiveTimeouts > 2) {
        _stats.queueOverflows++;
        _stats.ownRequestsDiscarded++;
        
        // Throttle warning message - only log once per 60 seconds
        unsigned long nowMs = millis();
        if ((nowMs - _lastTimeoutWarningMs) >= 60000) {
            LOG_W("Modbus: Queueing paused - %u consecutive timeouts (discarding requests)",
                  _consecutiveTimeouts);
            _lastTimeoutWarningMs = nowMs;
        }
        return false;
    }
    
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

uint16_t ModbusRTUFeature::calculateCRC(const uint8_t* data, size_t length) {
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
