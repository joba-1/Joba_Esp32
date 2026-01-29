#include "ModbusRTUFeature.h"
#include <esp_heap_caps.h>

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
    if (config == SERIAL_8E1 || config == SERIAL_8O1) bitsPerChar = 11;
    if (config == SERIAL_8N2 || config == SERIAL_8E2 || config == SERIAL_8O2) bitsPerChar = 11;
    
    _charTimeUs = (bitsPerChar * 1000000UL) / baudRate;
    
    // Modbus spec: 3.5 character times silence between frames
    // At baud rates > 19200, use fixed 1.75ms
    if (baudRate > 19200) {
        _silenceTimeUs = 1750;
    } else {
        _silenceTimeUs = _charTimeUs * 35 / 10;  // 3.5 char times
    }
    
    // Initialize interval stats start time
    _intervalStats.intervalStartMs = millis();
    
    _rxBuffer.reserve(256);
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
    
    // Read available data
    while (_serial.available()) {
        uint8_t byte = _serial.read();
        
        // Check for inter-character timeout (1.5 char times = new frame start)
        if (_rxBuffer.size() > 0 && (nowUs - _lastByteTime) > (_charTimeUs * 15 / 10)) {
            // Process previous frame before starting new one
            processReceivedData();
        }
        
        _rxBuffer.push_back(byte);
        _lastByteTime = nowUs;
        _lastActivityTime = nowMs;
        _busSilent = false;
        
        // Start tracking active time if not already
        if (!_inActiveTime && !_waitingForResponse) {
            startActiveTime(false);  // Assume other device traffic
        }
    }
    
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
        LOG_W("Modbus response timeout for unit %d FC 0x%02X", 
              _lastRequest.unitId, _lastRequest.functionCode);
        _stats.timeouts++;
        _stats.ownRequestsFailed++;
        _intervalStats.ownFailed++;
        
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
    if (_rxBuffer.size() < 4) {  // Minimum frame: ID + FC + CRC(2)
        _rxBuffer.clear();
        return;
    }
    
    ModbusFrame frame;
    if (parseFrame(_rxBuffer.data(), _rxBuffer.size(), frame)) {
        _stats.framesReceived++;
        
        // Determine if this is a request or response
        bool isRequest = false;
        bool isOurResponse = false;
        
        if (_waitingForResponse && frame.unitId == _lastRequest.unitId) {
            // This is a response to our request
            isRequest = false;
            isOurResponse = true;
            _waitingForResponse = false;
            
            // Reset consecutive timeout counter on any successful response (even with exception)
            _consecutiveTimeouts = 0;
            _lastSuccessTime = millis();
            
            // Track success/failure for our requests
            if (frame.isValid && !frame.isException) {
                _stats.ownRequestsSuccess++;
                _intervalStats.ownSuccess++;
            } else {
                _stats.ownRequestsFailed++;
                _intervalStats.ownFailed++;
                if (frame.isException) {
                    LOG_W("Modbus exception 0x%02X from unit %d", 
                          frame.exceptionCode, frame.unitId);
                }
            }
            
            // Update register map with response data
            updateRegisterMap(_lastRequest, frame);
            
            // Extract callback from the current request copy (safe, not from vector)
            std::function<void(bool, const ModbusFrame&)> callbackCopy = nullptr;
            if (_hasPendingRequest && _currentRequest.callback) {
                callbackCopy = _currentRequest.callback;
            }
            
            // Clear state immediately
            _hasPendingRequest = false;
            
            // NOW invoke the callback safely (outside the critical section)
            if (callbackCopy) {
                try {
                    callbackCopy(frame.isValid && !frame.isException, frame);
                } catch (...) {
                    LOG_E("Exception in Modbus response callback");
                }
            }
            
            // End our active time period
            endActiveTime();
            
        } else {
            // This is either a request from another master or response we're monitoring
            // Heuristic: requests typically have FC 0x01-0x06 with specific data lengths
            uint8_t fc = frame.functionCode & 0x7F;
            if (fc >= 0x01 && fc <= 0x04 && frame.data.size() == 4) {
                isRequest = true;
                _lastRequest = frame;
                _stats.otherRequestsSeen++;
                startActiveTime(false);  // Start tracking other device's communication
            } else if (fc == 0x05 && frame.data.size() == 4) {
                isRequest = true;
                _stats.otherRequestsSeen++;
                startActiveTime(false);
            } else if (fc == 0x06 && frame.data.size() == 4) {
                isRequest = true;
                _stats.otherRequestsSeen++;
                startActiveTime(false);
            } else if (fc == 0x10 && frame.data.size() >= 5) {
                isRequest = true;
                _stats.otherRequestsSeen++;
                startActiveTime(false);
            } else {
                // Looks like a response (to someone else's request)
                if (frame.isException) {
                    _stats.otherExceptionsSeen++;
                    _intervalStats.otherFailed++;
                } else {
                    _stats.otherResponsesSeen++;
                    _intervalStats.otherSuccess++;
                }
            }
            
            // Update register map if this looks like a response
            if (!isRequest && !frame.isException) {
                updateRegisterMap(_lastRequest, frame);
            }
        }
        
        // Notify callback
        if (_frameCallback) {
            _frameCallback(frame, isRequest);
        }
        
    } else {
        _stats.crcErrors++;
        LOG_V("Modbus CRC error, len=%u", _rxBuffer.size());
    }
    
    _rxBuffer.clear();
}

bool ModbusRTUFeature::parseFrame(const uint8_t* data, size_t length, ModbusFrame& frame) {
    if (length < 4) return false;
    
    // Verify CRC
    uint16_t receivedCrc = data[length - 2] | (data[length - 1] << 8);
    uint16_t calculatedCrc = calculateCRC(data, length - 2);
    
    if (receivedCrc != calculatedCrc) {
        frame.isValid = false;
        return false;
    }
    
    frame.unitId = data[0];
    frame.functionCode = data[1];
    frame.crc = receivedCrc;
    frame.timestamp = millis();
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
    
    uint16_t key = makeMapKey(response.unitId, fc);
    
    // Create map if doesn't exist
    if (_registerMaps.find(key) == _registerMaps.end()) {
        ModbusRegisterMap newMap;
        newMap.unitId = response.unitId;
        newMap.functionCode = fc;
        newMap.lastUpdate = 0;
        newMap.requestCount = 0;
        newMap.responseCount = 0;
        newMap.errorCount = 0;
        _registerMaps[key] = newMap;
        LOG_I("Created register map for unit %u, FC 0x%02X", response.unitId, fc);
    }
    
    ModbusRegisterMap& regMap = _registerMaps[key];
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
    
    if (sendRequest(req)) {
        _stats.ownRequestsSent++;
        
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
