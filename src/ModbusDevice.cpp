#include "ModbusDevice.h"
#include <LittleFS.h>
#include "TimeUtils.h"
#include "InfluxLineProtocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static bool valuesDiffer(float a, float b) {
    // Avoid requiring <cmath>; keep this simple for embedded builds.
    float diff = a - b;
    if (diff < 0) diff = -diff;
    return diff > 0.0001f;
}

ModbusDeviceManager::ModbusDeviceManager(ModbusRTUFeature& modbus, StorageFeature& storage)
    : _modbus(modbus)
    , _storage(storage)
    , _currentPollUnit(0)
    , _currentPollIndex(0)
    , _valueChangeCallback(nullptr)
    , _mutex(nullptr)
{
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (_mutex == nullptr) {
        LOG_E("ModbusDeviceManager: failed to create mutex");
    }

    // Observe all frames on the bus to account for passive responses
    _modbus.onFrame([this](const ModbusFrame& frame, bool isRequest) {
        handleObservedFrame(frame, isRequest);
    });
}

ModbusDeviceManager::ScopedLock::ScopedLock(SemaphoreHandle_t mutex)
    : _mutex(mutex) {
    if (_mutex) {
        (void)xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
    }
}

ModbusDeviceManager::ScopedLock::~ScopedLock() {
    if (_mutex) {
        (void)xSemaphoreGiveRecursive(_mutex);
    }
}

ModbusDeviceManager::ScopedLock::ScopedLock(ScopedLock&& other) noexcept
    : _mutex(other._mutex) {
    other._mutex = nullptr;
}

ModbusDeviceManager::ScopedLock& ModbusDeviceManager::ScopedLock::operator=(ScopedLock&& other) noexcept {
    if (this != &other) {
        if (_mutex) {
            (void)xSemaphoreGiveRecursive(_mutex);
        }
        _mutex = other._mutex;
        other._mutex = nullptr;
    }
    return *this;
}

ModbusDeviceManager::ScopedLock ModbusDeviceManager::scopedLock() const {
    return ScopedLock(_mutex);
}

void ModbusDeviceManager::handleObservedFrame(const ModbusFrame& frame, bool isRequest) {
    auto _guard = scopedLock();
    // Track last seen request per unit (used to infer address range for a later response)
    if (isRequest && frame.isValid) {
        _lastSeenRequests[frame.unitId] = frame;
        return;
    }

    // Responses: update counters and (best-effort) cached values
    auto it = _devices.find(frame.unitId);
    if (it == _devices.end()) return;

    if (!frame.isValid) {
        // CRC-invalid frames can't be trusted (unitId/functionCode may be garbage),
        // and are already tracked globally in ModbusRTU stats.
        return;
    }

    if (!frame.isException) {
        it->second.successCount++;

        // Best-effort: update cached register values if we can infer the request range
        auto reqIt = _lastSeenRequests.find(frame.unitId);
        if (reqIt != _lastSeenRequests.end()) {
            const ModbusFrame& request = reqIt->second;
            // Only consider reasonably recent request/response pairs
            if ((frame.timestamp - request.timestamp) < 2000) {
                tryUpdateFromPassiveResponse(it->second, request, frame);
            }
        }
    } else {
        // Exception response is a real device-level error
        it->second.errorCount++;
    }
}

void ModbusDeviceManager::tryUpdateFromPassiveResponse(ModbusDeviceInstance& device,
                                                       const ModbusFrame& request,
                                                       const ModbusFrame& response) {
    if (!device.deviceType) return;
    if (!request.isValid || !response.isValid) return;
    if (response.isException) return;

    // Only handle read responses where we can map bytes -> registers.
    uint8_t fc = response.functionCode & 0x7F;
    if (fc != ModbusFC::READ_HOLDING_REGISTERS && fc != ModbusFC::READ_INPUT_REGISTERS) return;
    if ((request.functionCode & 0x7F) != fc) return;

    const uint8_t* regData = response.getRegisterData();
    size_t byteCount = response.getByteCount();
    if (!regData || byteCount < 2) return;

    uint16_t startReg = request.getStartRegister();
    size_t respRegCount = byteCount / 2;
    if (respRegCount == 0) return;

    bool matchedAny = false;

    // Update any defined registers that fall fully within this response range
    for (const auto& reg : device.deviceType->registers) {
        if (reg.functionCode != fc) continue;
        if (reg.dataType == ModbusDataType::STRING) continue;  // Not supported here
        if (reg.address < startReg) continue;

        uint32_t offset = (uint32_t)(reg.address - startReg);
        if (offset + reg.length > respRegCount) continue;

        std::vector<uint16_t> rawData;
        rawData.reserve(reg.length);
        for (size_t i = 0; i < reg.length; i++) {
            size_t byteIndex = (offset + i) * 2;
            uint16_t word = ((uint16_t)regData[byteIndex] << 8) | (uint16_t)regData[byteIndex + 1];
            rawData.push_back(word);
        }

        float value = convertRawToValue(reg, rawData.data());

        auto& cached = device.currentValues[reg.name];
        bool shouldNotify = (!cached.valid) || valuesDiffer(cached.value, value);

        cached.updatedAtMs = millis();
        cached.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
        cached.timestamp = (cached.unixTimestamp != 0) ? cached.unixTimestamp : (cached.updatedAtMs / 1000);
        cached.value = value;
        cached.valid = true;

        matchedAny = true;

        if (shouldNotify) {
            notifyValueChange(device.unitId, reg.name, value, reg.unit);
        }
    }

    // If nothing in the JSON definition matched this request/response pair,
    // interpret it as unknown uint16 registers and report those.
    if (!matchedAny) {
        static constexpr size_t MAX_UNKNOWN_U16_PER_DEVICE = 512;

        for (size_t i = 0; i < respRegCount; i++) {
            uint16_t address = (uint16_t)(startReg + i);

            // Respect cap to avoid unbounded memory growth if a master scans huge ranges.
            auto existing = device.unknownU16.find(address);
            if (existing == device.unknownU16.end() && device.unknownU16.size() >= MAX_UNKNOWN_U16_PER_DEVICE) {
                break;
            }

            size_t byteIndex = i * 2;
            uint16_t word = ((uint16_t)regData[byteIndex] << 8) | (uint16_t)regData[byteIndex + 1];

            ModbusRegisterValue& v = device.unknownU16[address];
            v.updatedAtMs = millis();
            v.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
            v.timestamp = (v.unixTimestamp != 0) ? v.unixTimestamp : (v.updatedAtMs / 1000);
            snprintf(v.name, sizeof(v.name), "U16_%u", (unsigned)address);
            v.value = (float)word;
            v.unit[0] = '\0';
            v.valid = true;
        }
    }
}

bool ModbusDeviceManager::loadDeviceType(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        LOG_E("Failed to open device type: %s", path);
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        LOG_E("JSON parse error: %s", error.c_str());
        return false;
    }
    
    ModbusDeviceType deviceType;
    strlcpy(deviceType.name, doc["name"] | "unknown", sizeof(deviceType.name));
    
    JsonArray registers = doc["registers"].as<JsonArray>();
    for (JsonObject reg : registers) {
        ModbusRegisterDef def;
        strlcpy(def.name, reg["name"] | "", sizeof(def.name));
        def.address = reg["address"] | 0;
        def.length = reg["length"] | 1;
        def.functionCode = reg["functionCode"] | 3;
        def.dataType = parseDataType(reg["dataType"] | "uint16");
        def.conversionFactor = reg["factor"] | 1.0f;
        def.offset = reg["offset"] | 0.0f;
        strlcpy(def.unit, reg["unit"] | "", sizeof(def.unit));
        def.pollIntervalMs = reg["pollInterval"] | 0;
        
        deviceType.registers.push_back(def);
    }
    
    String key = deviceType.name;
    _deviceTypes[key] = deviceType;
    
    LOG_I("Loaded device type '%s' with %d registers", 
          deviceType.name, deviceType.registers.size());
    
    return true;
}

bool ModbusDeviceManager::loadDeviceMappings(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        LOG_E("Failed to open mappings: %s", path);
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        LOG_E("JSON parse error: %s", error.c_str());
        return false;
    }
    
    JsonArray devices = doc["devices"].as<JsonArray>();
    for (JsonObject dev : devices) {
        uint8_t unitId = dev["unitId"] | 0;
        const char* typeName = dev["type"] | "";
        const char* name = dev["name"] | "";
        
        auto typeIt = _deviceTypes.find(typeName);
        if (typeIt == _deviceTypes.end()) {
            LOG_W("Unknown device type '%s' for unit %d", typeName, unitId);
            continue;
        }
        
        ModbusDeviceInstance instance;
        instance.unitId = unitId;
        instance.deviceName = name;
        instance.deviceTypeName = typeName;
        instance.deviceType = &(typeIt->second);
        instance.lastPollTime = 0;
        instance.successCount = 0;
        instance.errorCount = 0;
        
        // Initialize values for all registers
        for (const auto& reg : instance.deviceType->registers) {
            ModbusRegisterValue val;
            val.timestamp = 0;
            val.updatedAtMs = 0;
            val.unixTimestamp = 0;
            strlcpy(val.name, reg.name, sizeof(val.name));
            val.value = 0;
            strlcpy(val.unit, reg.unit, sizeof(val.unit));
            val.valid = false;
            instance.currentValues[reg.name] = val;
        }

        // Precompute batched poll windows to avoid queue floods.
        rebuildPollBatches(instance);
        
        _devices[unitId] = instance;
        
        LOG_I("Mapped unit %d as '%s' (%s)",
              unitId, name, typeName);
    }
    
    return true;
}

void ModbusDeviceManager::rebuildPollBatches(ModbusDeviceInstance& device) {
    device.pollBatches.clear();
    if (!device.deviceType) return;

    // Only merge strictly contiguous ranges by default (no holes), to avoid reading
    // undocumented registers. This can be relaxed later if desired.
    static constexpr uint16_t GAP_ALLOW_REGS = 0;
    static constexpr uint16_t MAX_REGS_PER_READ = 125;  // Modbus RTU limit for FC3/FC4

    struct Seg {
        uint16_t start;
        uint16_t end;
        uint8_t fc;
        uint32_t interval;
    };

    std::vector<Seg> segs;
    segs.reserve(device.deviceType->registers.size());

    for (const auto& reg : device.deviceType->registers) {
        if (reg.pollIntervalMs == 0) continue;
        uint16_t start = reg.address;
        uint16_t end = (uint16_t)(reg.address + reg.length - 1);
        segs.push_back({start, end, reg.functionCode, reg.pollIntervalMs});
    }

    if (segs.empty()) return;

    // Sort by (fc, interval, start)
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
        if (a.fc != b.fc) return a.fc < b.fc;
        if (a.interval != b.interval) return a.interval < b.interval;
        return a.start < b.start;
    });

    // Merge into windows
    uint8_t curFc = segs[0].fc;
    uint32_t curInterval = segs[0].interval;
    uint16_t ws = segs[0].start;
    uint16_t we = segs[0].end;

    auto flushWindow = [&]() {
        uint16_t qty = (uint16_t)(we - ws + 1);
        if (qty == 0) return;
        device.pollBatches.push_back({curFc, ws, qty, curInterval, 0, 0});
    };

    for (size_t i = 1; i < segs.size(); i++) {
        const Seg& s = segs[i];
        if (s.fc != curFc || s.interval != curInterval) {
            flushWindow();
            curFc = s.fc;
            curInterval = s.interval;
            ws = s.start;
            we = s.end;
            continue;
        }

        uint16_t mergedEnd = (s.end > we) ? s.end : we;
        uint16_t mergedLen = (uint16_t)(mergedEnd - ws + 1);
        bool canMerge = (s.start <= (uint16_t)(we + 1 + GAP_ALLOW_REGS)) && (mergedLen <= MAX_REGS_PER_READ);

        if (canMerge) {
            we = mergedEnd;
        } else {
            flushWindow();
            ws = s.start;
            we = s.end;
        }
    }
    flushWindow();

    LOG_I("Modbus poll plan for unit %u: %u batched windows", device.unitId, (unsigned)device.pollBatches.size());
}

void ModbusDeviceManager::applyReadResponseToDevice(ModbusDeviceInstance& device,
                                                    uint8_t functionCode,
                                                    uint32_t pollIntervalMs,
                                                    uint16_t startAddress,
                                                    const ModbusFrame& response) {
    auto _guard = scopedLock();
    if (!device.deviceType) return;
    if (!response.isValid || response.isException) return;

    const uint8_t* data = response.getRegisterData();
    size_t byteCount = response.getByteCount();
    if (!data || byteCount < 2) return;

    const uint16_t wordCount = (uint16_t)(byteCount / 2);

    // Convert response bytes into 16-bit words (big-endian on the wire)
    std::vector<uint16_t> words;
    words.reserve(wordCount);
    for (uint16_t i = 0; i < wordCount; i++) {
        uint16_t w = ((uint16_t)data[i * 2] << 8) | (uint16_t)data[i * 2 + 1];
        words.push_back(w);
    }

    const uint32_t nowMs = millis();
    const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();

    // Update every register definition that is covered by this read window.
    for (const auto& reg : device.deviceType->registers) {
        if (reg.pollIntervalMs != pollIntervalMs) continue;
        if (reg.functionCode != functionCode) continue;
        if (reg.dataType == ModbusDataType::STRING) continue;

        if (reg.address < startAddress) continue;
        uint32_t offset = (uint32_t)(reg.address - startAddress);
        if (offset + reg.length > wordCount) continue;

        float value = convertRawToValue(reg, &words[offset]);

        auto& cached = device.currentValues[reg.name];
        cached.updatedAtMs = nowMs;
        cached.unixTimestamp = nowUnix;
        cached.timestamp = (nowUnix != 0) ? nowUnix : (nowMs / 1000);
        cached.value = value;
        cached.valid = true;

        notifyValueChange(device.unitId, reg.name, value, reg.unit);
    }
}

bool ModbusDeviceManager::loadAllDeviceTypes(const char* directory) {
    LOG_D("loadAllDeviceTypes: scanning %s", directory);
    
    // Try direct directory open first
    File dir = LittleFS.open(directory);
    if (dir && dir.isDirectory()) {
        LOG_D("Directory %s exists as explicit LittleFS directory", directory);
        int count = 0;
        File file = dir.openNextFile();
        while (file) {
            if (!file.isDirectory() && String(file.name()).endsWith(".json")) {
                String path = String(directory) + "/" + file.name();
                if (loadDeviceType(path.c_str())) {
                    count++;
                }
            }
            file = dir.openNextFile();
        }
        
        LOG_I("Loaded %d device types from %s", count, directory);
        return count > 0;
    }
    
    // Fallback: scan root filesystem for files matching the directory prefix
    // This handles cases where intermediate directories exist only implicitly via file paths
    LOG_D("Directory %s not found as explicit directory, scanning filesystem", directory);
    
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        LOG_E("Failed to scan root filesystem");
        return false;
    }
    
    String prefix = String(directory);
    if (!prefix.endsWith("/")) prefix += "/";
    
    int count = 0;
    File file = root.openNextFile();
    while (file) {
        String fname = String(file.name());
        if (!file.isDirectory() && fname.startsWith(prefix) && fname.endsWith(".json")) {
            // Check this is a direct child (no additional slashes)
            String tail = fname.substring(prefix.length());
            if (tail.indexOf('/') == -1) {
                LOG_D("Found device file: %s", fname.c_str());
                if (loadDeviceType(fname.c_str())) {
                    count++;
                }
            }
        }
        file = root.openNextFile();
    }
    
    LOG_I("Loaded %d device types from %s (via filesystem scan)", count, directory);
    return count > 0;
}

const ModbusDeviceType* ModbusDeviceManager::getDeviceType(const char* name) const {
    auto it = _deviceTypes.find(name);
    if (it != _deviceTypes.end()) {
        return &(it->second);
    }
    return nullptr;
}

ModbusDeviceInstance* ModbusDeviceManager::getDevice(uint8_t unitId) {
    auto it = _devices.find(unitId);
    if (it != _devices.end()) {
        return &(it->second);
    }
    return nullptr;
}

const ModbusRegisterDef* ModbusDeviceManager::findRegister(const ModbusDeviceType* type, const char* name) const {
    if (!type) return nullptr;
    for (const auto& reg : type->registers) {
        if (strcmp(reg.name, name) == 0) {
            return &reg;
        }
    }
    return nullptr;
}

bool ModbusDeviceManager::readRegister(uint8_t unitId, const char* registerName,
                                       std::function<void(bool, float)> callback) {
    // Note: This function can be called from the AsyncWebServer task.
    // Protect shared device state (maps) against concurrent Modbus polling updates.
    auto _guard = scopedLock();
    auto* device = getDevice(unitId);
    if (!device || !device->deviceType) {
        LOG_E("Unknown device unit %d", unitId);
        if (callback) callback(false, 0);
        return false;
    }
    
    const ModbusRegisterDef* reg = findRegister(device->deviceType, registerName);
    if (!reg) {
        LOG_E("Unknown register '%s'", registerName);
        if (callback) callback(false, 0);
        return false;
    }
    
    // Queue the read request
    return _modbus.queueReadRegisters(unitId, reg->functionCode, reg->address, reg->length,
        [this, device, reg, callback](bool success, const ModbusFrame& response) {
            auto _guard = scopedLock();
            float value = 0;
            if (success && !response.isException) {
                // Extract register data from response
                const uint8_t* data = response.getRegisterData();
                size_t byteCount = response.getByteCount();
                
                if (data && byteCount >= reg->length * 2) {
                    std::vector<uint16_t> rawData;
                    for (size_t i = 0; i < reg->length; i++) {
                        rawData.push_back((data[i*2] << 8) | data[i*2 + 1]);
                    }
                    value = convertRawToValue(*reg, rawData.data());
                    
                    // Update cached value
                    auto& cached = device->currentValues[reg->name];
                    cached.updatedAtMs = millis();
                    cached.unixTimestamp = TimeUtils::nowUnixSecondsOrZero();
                    cached.timestamp = (cached.unixTimestamp != 0) ? cached.unixTimestamp : (cached.updatedAtMs / 1000);
                    cached.value = value;
                    cached.valid = true;
                    
                    // Notify value change callback
                    notifyValueChange(device->unitId, reg->name, value, reg->unit);
                } else {
                    device->errorCount++;
                    auto& cached = device->currentValues[reg->name];
                    cached.valid = false;
                }
            } else {
                device->errorCount++;
                auto& cached = device->currentValues[reg->name];
                cached.valid = false;
            }
            
            if (callback) callback(success, value);
        });
}

bool ModbusDeviceManager::readAllRegisters(uint8_t unitId,
                                           std::function<void(bool)> callback) {
    auto _guard = scopedLock();
    auto* device = getDevice(unitId);
    if (!device || !device->deviceType) {
        if (callback) callback(false);
        return false;
    }
    
    // Queue reads for all registers
    for (const auto& reg : device->deviceType->registers) {
        readRegister(unitId, reg.name, nullptr);
    }
    
    // Note: callback doesn't wait for all reads to complete
    // For proper completion handling, use individual callbacks
    if (callback) callback(true);
    return true;
}

bool ModbusDeviceManager::writeRegister(uint8_t unitId, const char* registerName,
                                        float value, std::function<void(bool)> callback) {
    auto _guard = scopedLock();
    auto* device = getDevice(unitId);
    if (!device || !device->deviceType) {
        if (callback) callback(false);
        return false;
    }
    
    const ModbusRegisterDef* reg = findRegister(device->deviceType, registerName);
    if (!reg) {
        if (callback) callback(false);
        return false;
    }
    
    auto rawValues = convertValueToRaw(*reg, value);
    
    if (rawValues.size() == 1) {
        return _modbus.queueWriteSingleRegister(unitId, reg->address, rawValues[0],
            [callback](bool success, const ModbusFrame&) {
                if (callback) callback(success);
            });
    } else {
        return _modbus.queueWriteMultipleRegisters(unitId, reg->address, rawValues,
            [callback](bool success, const ModbusFrame&) {
                if (callback) callback(success);
            });
    }
}

bool ModbusDeviceManager::writeRawRegister(uint8_t unitId, uint8_t functionCode,
                                           uint16_t address, uint16_t value,
                                           std::function<void(bool)> callback) {
    return _modbus.queueWriteSingleRegister(unitId, address, value,
        [callback](bool success, const ModbusFrame&) {
            if (callback) callback(success);
        });
}

bool ModbusDeviceManager::writeRawRegisters(uint8_t unitId, uint16_t startAddress,
                                            const std::vector<uint16_t>& values,
                                            std::function<void(bool)> callback) {
    return _modbus.queueWriteMultipleRegisters(unitId, startAddress, values,
        [callback](bool success, const ModbusFrame&) {
            if (callback) callback(success);
        });
}

bool ModbusDeviceManager::getValue(uint8_t unitId, const char* registerName, float& value) const {
    auto _guard = scopedLock();
    auto it = _devices.find(unitId);
    if (it == _devices.end()) return false;
    
    auto valIt = it->second.currentValues.find(registerName);
    if (valIt == it->second.currentValues.end()) return false;
    
    if (!valIt->second.valid) return false;
    
    value = valIt->second.value;
    return true;
}

String ModbusDeviceManager::getDeviceValuesJson(uint8_t unitId) const {
    // Snapshot under lock (no heavy allocations while holding mutex).
    // Keep this small: unknown registers can get large quickly and this JSON is served from heap-backed buffers.
    static constexpr size_t MAX_UNKNOWN_U16_JSON = 32;
    char deviceTypeName[64] = {0};
    uint32_t successCount = 0;
    uint32_t errorCount = 0;
    size_t valuesCount = 0;
    size_t unknownCount = 0;

    {
        auto _guard = scopedLock();
        auto it = _devices.find(unitId);
        if (it == _devices.end()) {
            JsonDocument err;
            err["error"] = "Device not found";
            String output;
            serializeJson(err, output);
            return output;
        }

        const auto& device = it->second;
        strlcpy(deviceTypeName, device.deviceTypeName.c_str(), sizeof(deviceTypeName));
        successCount = device.successCount;
        errorCount = device.errorCount;
        valuesCount = device.currentValues.size();
        unknownCount = device.unknownU16.size();
    }

    std::vector<ModbusRegisterValue> values;
    values.reserve(valuesCount);
    struct UnknownItem {
        uint16_t address;
        ModbusRegisterValue value;
    };
    std::vector<UnknownItem> unknown;
    unknown.reserve(std::min(unknownCount, MAX_UNKNOWN_U16_JSON));

    size_t unknownTotal = 0;
    {
        auto _guard = scopedLock();
        auto it = _devices.find(unitId);
        if (it == _devices.end()) {
            JsonDocument err;
            err["error"] = "Device not found";
            String output;
            serializeJson(err, output);
            return output;
        }

        const auto& device = it->second;
        for (const auto& kv : device.currentValues) {
            values.push_back(kv.second);
        }
        unknownTotal = device.unknownU16.size();
        size_t emitted = 0;
        for (const auto& kv : device.unknownU16) {
            if (emitted >= MAX_UNKNOWN_U16_JSON) break;
            unknown.push_back(UnknownItem{kv.first, kv.second});
            emitted++;
        }
    }

    const bool timeValid = TimeUtils::isTimeValidNow();
    const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();

    JsonDocument doc;
    doc["unitId"] = unitId;
    doc["deviceType"] = deviceTypeName;
    doc["successCount"] = successCount;
    doc["errorCount"] = errorCount;

    {
        JsonObject updated = doc["updated"].to<JsonObject>();
        updated["uptimeMs"] = (uint32_t)millis();
        if (nowUnix != 0) {
            updated["epoch"] = nowUnix;
            String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
            if (iso.length() > 0) updated["iso"] = iso;
        }
    }

    doc["valuesCount"] = (uint32_t)values.size();
    JsonArray valuesArr = doc["values"].to<JsonArray>();
    for (const auto& v : values) {
        JsonObject val = valuesArr.add<JsonObject>();
        val["name"] = v.name;
        val["value"] = v.value;
        val["unit"] = v.unit;
        val["valid"] = v.valid;

        JsonObject updated = val["updated"].to<JsonObject>();
        updated["uptimeMs"] = v.updatedAtMs;
        if (v.unixTimestamp != 0) {
            updated["epoch"] = v.unixTimestamp;
        } else if (timeValid && nowUnix != 0 && v.updatedAtMs != 0) {
            uint32_t uptimeSeconds = v.updatedAtMs / 1000;
            uint32_t estEpoch = TimeUtils::unixFromUptimeSeconds(uptimeSeconds);
            if (estEpoch != 0) {
                updated["epoch"] = estEpoch;
            }
        }
    }

    doc["unknownU16Count"] = (uint32_t)unknownTotal;
    doc["unknownU16Limit"] = (uint32_t)MAX_UNKNOWN_U16_JSON;
    JsonArray unknownArr = doc["unknownU16"].to<JsonArray>();
    for (const auto& u : unknown) {
        JsonObject val = unknownArr.add<JsonObject>();
        val["address"] = u.address;
        val["name"] = u.value.name;
        val["value"] = u.value.value;
        val["valid"] = u.value.valid;

        JsonObject updated = val["updated"].to<JsonObject>();
        updated["uptimeMs"] = u.value.updatedAtMs;
        if (u.value.unixTimestamp != 0) {
            updated["epoch"] = u.value.unixTimestamp;
        } else if (timeValid && nowUnix != 0 && u.value.updatedAtMs != 0) {
            uint32_t uptimeSeconds = u.value.updatedAtMs / 1000;
            uint32_t estEpoch = TimeUtils::unixFromUptimeSeconds(uptimeSeconds);
            if (estEpoch != 0) {
                updated["epoch"] = estEpoch;
            }
        }
    }
    doc["unknownU16Truncated"] = (bool)(unknownTotal > unknown.size());

    String output;
    serializeJson(doc, output);
    return output;
}

void ModbusDeviceManager::writeDeviceValuesJson(uint8_t unitId, Print& out) const {
    // Snapshot under lock (avoid holding mutex while doing JSON allocations/serialization).
    // Keep this small: unknown registers can get large quickly and this JSON is served from heap-backed buffers.
    static constexpr size_t MAX_UNKNOWN_U16_JSON = 32;
    char deviceTypeName[64] = {0};
    uint32_t successCount = 0;
    uint32_t errorCount = 0;
    size_t valuesCount = 0;
    size_t unknownCount = 0;

    {
        auto _guard = scopedLock();
        auto it = _devices.find(unitId);
        if (it == _devices.end()) {
            JsonDocument err;
            err["error"] = "Device not found";
            serializeJson(err, out);
            return;
        }

        const auto& device = it->second;
        strlcpy(deviceTypeName, device.deviceTypeName.c_str(), sizeof(deviceTypeName));
        successCount = device.successCount;
        errorCount = device.errorCount;
        valuesCount = device.currentValues.size();
        unknownCount = device.unknownU16.size();
    }

    std::vector<ModbusRegisterValue> values;
    values.reserve(valuesCount);
    struct UnknownItem {
        uint16_t address;
        ModbusRegisterValue value;
    };
    std::vector<UnknownItem> unknown;
    unknown.reserve(std::min(unknownCount, MAX_UNKNOWN_U16_JSON));

    size_t unknownTotal = 0;
    {
        auto _guard = scopedLock();
        auto it = _devices.find(unitId);
        if (it == _devices.end()) {
            JsonDocument err;
            err["error"] = "Device not found";
            serializeJson(err, out);
            return;
        }

        const auto& device = it->second;
        for (const auto& kv : device.currentValues) {
            values.push_back(kv.second);
        }
        unknownTotal = device.unknownU16.size();
        size_t emitted = 0;
        for (const auto& kv : device.unknownU16) {
            if (emitted >= MAX_UNKNOWN_U16_JSON) break;
            unknown.push_back(UnknownItem{kv.first, kv.second});
            emitted++;
        }
    }

    const bool timeValid = TimeUtils::isTimeValidNow();
    const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();

    // Keep the JSON shape identical to getDeviceValuesJson(), but avoid building a large String.
    JsonDocument doc;
    doc["unitId"] = unitId;
    doc["deviceType"] = deviceTypeName;
    doc["successCount"] = successCount;
    doc["errorCount"] = errorCount;

    {
        JsonObject updated = doc["updated"].to<JsonObject>();
        updated["uptimeMs"] = (uint32_t)millis();
        if (nowUnix != 0) {
            updated["epoch"] = nowUnix;
            String iso = TimeUtils::isoUtcFromUnixSeconds(nowUnix);
            if (iso.length() > 0) updated["iso"] = iso;
        }
    }

    doc["valuesCount"] = (uint32_t)values.size();
    JsonArray valuesArr = doc["values"].to<JsonArray>();
    for (const auto& v : values) {
        JsonObject val = valuesArr.add<JsonObject>();
        val["name"] = v.name;
        val["value"] = v.value;
        val["unit"] = v.unit;
        val["valid"] = v.valid;

        JsonObject updated = val["updated"].to<JsonObject>();
        updated["uptimeMs"] = v.updatedAtMs;
        if (v.unixTimestamp != 0) {
            updated["epoch"] = v.unixTimestamp;
        } else if (timeValid && nowUnix != 0 && v.updatedAtMs != 0) {
            uint32_t uptimeSeconds = v.updatedAtMs / 1000;
            uint32_t estEpoch = TimeUtils::unixFromUptimeSeconds(uptimeSeconds);
            if (estEpoch != 0) {
                updated["epoch"] = estEpoch;
            }
        }
    }

    doc["unknownU16Count"] = (uint32_t)unknownTotal;
    doc["unknownU16Limit"] = (uint32_t)MAX_UNKNOWN_U16_JSON;
    JsonArray unknownArr = doc["unknownU16"].to<JsonArray>();
    for (const auto& u : unknown) {
        JsonObject val = unknownArr.add<JsonObject>();
        val["address"] = u.address;
        val["name"] = u.value.name;
        val["value"] = u.value.value;
        val["valid"] = u.value.valid;

        JsonObject updated = val["updated"].to<JsonObject>();
        updated["uptimeMs"] = u.value.updatedAtMs;
        if (u.value.unixTimestamp != 0) {
            updated["epoch"] = u.value.unixTimestamp;
        } else if (timeValid && nowUnix != 0 && u.value.updatedAtMs != 0) {
            uint32_t uptimeSeconds = u.value.updatedAtMs / 1000;
            uint32_t estEpoch = TimeUtils::unixFromUptimeSeconds(uptimeSeconds);
            if (estEpoch != 0) {
                updated["epoch"] = estEpoch;
            }
        }
    }

    doc["unknownU16Truncated"] = (bool)(unknownTotal > unknown.size());
    serializeJson(doc, out);
}

void ModbusDeviceManager::writeDeviceMetaJson(uint8_t unitId, Print& out) const {
    char deviceTypeName[64] = {0};
    uint32_t successCount = 0;
    uint32_t errorCount = 0;
    uint32_t valuesCount = 0;
    uint32_t unknownCount = 0;

    {
        auto _guard = scopedLock();
        auto it = _devices.find(unitId);
        if (it == _devices.end()) {
            JsonDocument err;
            err["error"] = "Device not found";
            serializeJson(err, out);
            return;
        }

        const auto& device = it->second;
        strlcpy(deviceTypeName, device.deviceTypeName.c_str(), sizeof(deviceTypeName));
        successCount = device.successCount;
        errorCount = device.errorCount;
        valuesCount = (uint32_t)device.currentValues.size();
        unknownCount = (uint32_t)device.unknownU16.size();
    }

    JsonDocument doc;
    doc["unitId"] = unitId;
    doc["deviceType"] = deviceTypeName;
    doc["successCount"] = successCount;
    doc["errorCount"] = errorCount;
    doc["valuesCount"] = valuesCount;
    doc["unknownU16Count"] = unknownCount;

    JsonObject updated = doc["updated"].to<JsonObject>();
    updated["uptimeMs"] = (uint32_t)millis();
    const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
    if (nowUnix != 0) updated["epoch"] = nowUnix;

    serializeJson(doc, out);
}

void ModbusDeviceManager::loop() {
#if MODBUS_LISTEN_ONLY
    return;
#endif
    auto _guard = scopedLock();
    if (_devices.empty()) return;

    const uint32_t now = (uint32_t)millis();

    // Avoid queue floods: only schedule a new poll when the Modbus queue is empty.
    // This also naturally adapts to a busy bus.
    if (_modbus.getPendingRequestCount() > 0) return;

    // If queueing is temporarily rejected (e.g. timeout backoff), don't hammer
    // queueReadRegisters() in a tight loop.
    static constexpr uint32_t QUEUE_RETRY_COOLDOWN_MS = 250;

    // Pick the batch with the earliest next-due time across all devices.
    ModbusDeviceInstance* bestDevice = nullptr;
    ModbusDeviceInstance::ModbusPollBatch* bestBatch = nullptr;
    uint32_t bestNextDue = 0;
    bool bestSet = false;

    for (auto& kv : _devices) {
        auto& device = kv.second;
        if (!device.deviceType) continue;
        if (device.pollBatches.empty()) {
            rebuildPollBatches(device);
        }
        for (auto& batch : device.pollBatches) {
            if (batch.pollIntervalMs == 0) continue;

            if (batch.lastAttemptMs != 0 && (uint32_t)(now - batch.lastAttemptMs) < QUEUE_RETRY_COOLDOWN_MS) {
                continue;
            }

            uint32_t nextDue = (batch.lastPollMs == 0) ? 0 : (uint32_t)(batch.lastPollMs + batch.pollIntervalMs);
            if (!bestSet || nextDue < bestNextDue) {
                bestSet = true;
                bestNextDue = nextDue;
                bestDevice = &device;
                bestBatch = &batch;
            }
        }
    }

    if (!bestSet || !bestDevice || !bestBatch) return;
    if (bestNextDue > now && bestBatch->lastPollMs != 0) return;  // Not due yet

    const uint8_t unitId = bestDevice->unitId;
    const uint8_t fc = bestBatch->functionCode;
    const uint16_t startAddr = bestBatch->startAddress;
    const uint16_t qty = bestBatch->quantity;
    const uint32_t interval = bestBatch->pollIntervalMs;

    bool queued = _modbus.queueReadRegisters(unitId, fc, startAddr, qty,
        [this, unitId, fc, interval, startAddr, qty](bool success, const ModbusFrame& response) {
            auto _guard = scopedLock();
            auto it = _devices.find(unitId);
            if (it == _devices.end()) return;
            auto& device = it->second;
            if (!device.deviceType) return;

            if (success && response.isValid && !response.isException) {
                device.successCount++;
                applyReadResponseToDevice(device, fc, interval, startAddr, response);
            } else {
                device.errorCount++;
                // Mark registers in this interval/functionCode as invalid if they are covered.
                // (Best-effort; avoids stale data being presented as fresh.)
                const uint32_t nowMs = millis();
                const uint32_t nowUnix = TimeUtils::nowUnixSecondsOrZero();
                for (const auto& reg : device.deviceType->registers) {
                    if (reg.pollIntervalMs != interval) continue;
                    if (reg.functionCode != fc) continue;
                    if (reg.address < startAddr) continue;
                    uint32_t offset = (uint32_t)(reg.address - startAddr);
                    if (offset + reg.length > (uint32_t)qty) continue;
                    auto& cached = device.currentValues[reg.name];
                    cached.updatedAtMs = nowMs;
                    cached.unixTimestamp = nowUnix;
                    cached.timestamp = (nowUnix != 0) ? nowUnix : (nowMs / 1000);
                    cached.valid = false;
                }
            }
        });

    // Always record the attempt, even if queueing failed, to avoid tight retry loops.
    bestBatch->lastAttemptMs = now;

    if (queued) {
        bestBatch->lastPollMs = now;
        bestDevice->lastPollTime = now;
    }
}

std::vector<String> ModbusDeviceManager::getDeviceTypeNames() const {
    std::vector<String> names;
    for (const auto& kv : _deviceTypes) {
        names.push_back(kv.first);
    }
    return names;
}

ModbusDataType ModbusDeviceManager::parseDataType(const char* str) const {
    if (strcasecmp(str, "int16") == 0) return ModbusDataType::INT16;
    if (strcasecmp(str, "uint32_be") == 0) return ModbusDataType::UINT32_BE;
    if (strcasecmp(str, "uint32_le") == 0) return ModbusDataType::UINT32_LE;
    if (strcasecmp(str, "int32_be") == 0) return ModbusDataType::INT32_BE;
    if (strcasecmp(str, "int32_le") == 0) return ModbusDataType::INT32_LE;
    if (strcasecmp(str, "float32_be") == 0) return ModbusDataType::FLOAT32_BE;
    if (strcasecmp(str, "float32_le") == 0) return ModbusDataType::FLOAT32_LE;
    if (strcasecmp(str, "bool") == 0) return ModbusDataType::BOOL;
    if (strcasecmp(str, "string") == 0) return ModbusDataType::STRING;
    return ModbusDataType::UINT16;  // Default
}

float ModbusDeviceManager::convertRawToValue(const ModbusRegisterDef& def,
                                             const uint16_t* rawData) const {
    float rawValue = 0;
    
    switch (def.dataType) {
        case ModbusDataType::UINT16:
            rawValue = rawData[0];
            break;
            
        case ModbusDataType::INT16:
            rawValue = (int16_t)rawData[0];
            break;
            
        case ModbusDataType::UINT32_BE:
            rawValue = ((uint32_t)rawData[0] << 16) | rawData[1];
            break;
            
        case ModbusDataType::UINT32_LE:
            rawValue = ((uint32_t)rawData[1] << 16) | rawData[0];
            break;
            
        case ModbusDataType::INT32_BE: {
            int32_t val = ((uint32_t)rawData[0] << 16) | rawData[1];
            rawValue = val;
            break;
        }
            
        case ModbusDataType::INT32_LE: {
            int32_t val = ((uint32_t)rawData[1] << 16) | rawData[0];
            rawValue = val;
            break;
        }
            
        case ModbusDataType::FLOAT32_BE: {
            uint32_t bits = ((uint32_t)rawData[0] << 16) | rawData[1];
            memcpy(&rawValue, &bits, sizeof(float));
            break;
        }
            
        case ModbusDataType::FLOAT32_LE: {
            uint32_t bits = ((uint32_t)rawData[1] << 16) | rawData[0];
            memcpy(&rawValue, &bits, sizeof(float));
            break;
        }
            
        case ModbusDataType::BOOL:
            rawValue = rawData[0] ? 1.0f : 0.0f;
            break;
            
        default:
            rawValue = rawData[0];
            break;
    }
    
    return (rawValue * def.conversionFactor) + def.offset;
}

std::vector<uint16_t> ModbusDeviceManager::convertValueToRaw(const ModbusRegisterDef& def,
                                                              float value) const {
    std::vector<uint16_t> result;
    
    // Reverse the conversion factor and offset
    float rawValue = (value - def.offset) / def.conversionFactor;
    
    switch (def.dataType) {
        case ModbusDataType::UINT16:
            result.push_back((uint16_t)rawValue);
            break;
            
        case ModbusDataType::INT16:
            result.push_back((uint16_t)(int16_t)rawValue);
            break;
            
        case ModbusDataType::UINT32_BE: {
            uint32_t val = (uint32_t)rawValue;
            result.push_back(val >> 16);
            result.push_back(val & 0xFFFF);
            break;
        }
            
        case ModbusDataType::UINT32_LE: {
            uint32_t val = (uint32_t)rawValue;
            result.push_back(val & 0xFFFF);
            result.push_back(val >> 16);
            break;
        }
            
        case ModbusDataType::INT32_BE: {
            int32_t val = (int32_t)rawValue;
            result.push_back((uint16_t)(val >> 16));
            result.push_back((uint16_t)(val & 0xFFFF));
            break;
        }
            
        case ModbusDataType::INT32_LE: {
            int32_t val = (int32_t)rawValue;
            result.push_back((uint16_t)(val & 0xFFFF));
            result.push_back((uint16_t)(val >> 16));
            break;
        }
            
        case ModbusDataType::FLOAT32_BE: {
            uint32_t bits;
            memcpy(&bits, &rawValue, sizeof(float));
            result.push_back(bits >> 16);
            result.push_back(bits & 0xFFFF);
            break;
        }
            
        case ModbusDataType::FLOAT32_LE: {
            uint32_t bits;
            memcpy(&bits, &rawValue, sizeof(float));
            result.push_back(bits & 0xFFFF);
            result.push_back(bits >> 16);
            break;
        }
            
        case ModbusDataType::BOOL:
            result.push_back(rawValue >= 0.5f ? 1 : 0);
            break;
            
        default:
            result.push_back((uint16_t)rawValue);
            break;
    }
    
    return result;
}

void ModbusDeviceManager::notifyValueChange(uint8_t unitId, const char* registerName,
                                             float value, const char* unit) {
    if (_valueChangeCallback) {
        auto it = _devices.find(unitId);
        if (it != _devices.end()) {
            _valueChangeCallback(unitId, it->second.deviceName.c_str(),
                                  registerName, value, unit);
        }
    }
}

String ModbusDeviceManager::toLineProtocol(uint8_t unitId, const char* measurement) const {
    auto it = _devices.find(unitId);
    if (it == _devices.end()) return "";
    
    const auto& device = it->second;
    String lines;
    
    for (const auto& kv : device.currentValues) {
        if (!kv.second.valid) continue;
        
        // Format: measurement,device=name,unit_id=N,register=RegName value=X timestamp
        String line = InfluxLineProtocol::escapeMeasurement(measurement);
        line += ",device=";
        line += InfluxLineProtocol::escapeTag(device.deviceName);
        line += ",unit_id=";
        line += String(unitId);
        line += ",register=";
        line += InfluxLineProtocol::escapeTag(kv.first);
        if (strlen(kv.second.unit) > 0) {
            line += ",unit=";
            line += InfluxLineProtocol::escapeTag(kv.second.unit);
        }
        line += " value=";
        line += String(kv.second.value, 4);
        if (kv.second.unixTimestamp != 0) {
            line += " ";
            line += String((uint64_t)kv.second.unixTimestamp * 1000000000ULL);  // ns
        }
        line += "\n";
        lines += line;
    }
    
    return lines;
}

std::vector<String> ModbusDeviceManager::allToLineProtocol(const char* measurement) const {
    std::vector<String> result;
    
    for (const auto& kv : _devices) {
        const auto& device = kv.second;
        
        for (const auto& regKv : device.currentValues) {
            if (!regKv.second.valid) continue;
            
            // Format: measurement,device=name,unit_id=N,register=RegName value=X timestamp
            String line = InfluxLineProtocol::escapeMeasurement(measurement);
            line += ",device=";
            line += InfluxLineProtocol::escapeTag(device.deviceName);
            line += ",unit_id=";
            line += String(device.unitId);
            line += ",register=";
            line += InfluxLineProtocol::escapeTag(regKv.first);
            if (strlen(regKv.second.unit) > 0) {
                line += ",unit=";
                line += InfluxLineProtocol::escapeTag(regKv.second.unit);
            }
            line += " value=";
            line += String(regKv.second.value, 4);
            if (regKv.second.unixTimestamp != 0) {
                line += " ";
                line += String((uint64_t)regKv.second.unixTimestamp * 1000000000ULL);  // ns
            }
            
            result.push_back(line);
        }
    }
    
    return result;
}
