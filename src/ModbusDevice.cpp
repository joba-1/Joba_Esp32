#include "ModbusDevice.h"
#include <LittleFS.h>

ModbusDeviceManager::ModbusDeviceManager(ModbusRTUFeature& modbus, StorageFeature& storage)
    : _modbus(modbus)
    , _storage(storage)
    , _currentPollUnit(0)
    , _currentPollIndex(0)
    , _valueChangeCallback(nullptr)
{
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
            strlcpy(val.name, reg.name, sizeof(val.name));
            val.value = 0;
            strlcpy(val.unit, reg.unit, sizeof(val.unit));
            val.valid = false;
            instance.currentValues[reg.name] = val;
        }
        
        _devices[unitId] = instance;
        
        LOG_I("Mapped unit %d as '%s' (%s)",
              unitId, name, typeName);
    }
    
    return true;
}

bool ModbusDeviceManager::loadAllDeviceTypes(const char* directory) {
    File dir = LittleFS.open(directory);
    if (!dir || !dir.isDirectory()) {
        LOG_E("Failed to open device directory: %s", directory);
        return false;
    }
    
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
                    cached.timestamp = millis() / 1000;
                    cached.value = value;
                    cached.valid = true;
                    device->successCount++;
                    
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
    auto it = _devices.find(unitId);
    if (it == _devices.end()) return false;
    
    auto valIt = it->second.currentValues.find(registerName);
    if (valIt == it->second.currentValues.end()) return false;
    
    if (!valIt->second.valid) return false;
    
    value = valIt->second.value;
    return true;
}

String ModbusDeviceManager::getDeviceValuesJson(uint8_t unitId) const {
    JsonDocument doc;
    
    auto it = _devices.find(unitId);
    if (it == _devices.end()) {
        doc["error"] = "Device not found";
        String output;
        serializeJson(doc, output);
        return output;
    }
    
    const auto& device = it->second;
    doc["unitId"] = unitId;
    doc["deviceType"] = device.deviceTypeName;
    doc["successCount"] = device.successCount;
    doc["errorCount"] = device.errorCount;
    
    JsonArray values = doc["values"].to<JsonArray>();
    for (const auto& kv : device.currentValues) {
        JsonObject val = values.add<JsonObject>();
        val["name"] = kv.second.name;
        val["value"] = kv.second.value;
        val["unit"] = kv.second.unit;
        val["valid"] = kv.second.valid;
        val["timestamp"] = kv.second.timestamp;
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

void ModbusDeviceManager::loop() {
    if (_devices.empty()) return;
    
    unsigned long now = millis();
    
    // Check each device for poll intervals
    for (auto& kv : _devices) {
        auto& device = kv.second;
        if (!device.deviceType) continue;
        
        for (const auto& reg : device.deviceType->registers) {
            if (reg.pollIntervalMs == 0) continue;  // On-demand only
            
            auto& cached = device.currentValues[reg.name];
            unsigned long lastPoll = cached.timestamp * 1000;
            
            if (now - lastPoll >= reg.pollIntervalMs) {
                // Time to poll this register
                readRegister(device.unitId, reg.name, nullptr);
            }
        }
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
        String line = measurement;
        line += ",device=";
        line += device.deviceName;
        line += ",unit_id=";
        line += String(unitId);
        line += ",register=";
        line += kv.first;
        if (strlen(kv.second.unit) > 0) {
            line += ",unit=";
            line += kv.second.unit;
        }
        line += " value=";
        line += String(kv.second.value, 4);
        line += " ";
        line += String((uint64_t)kv.second.timestamp * 1000000000ULL);  // ns
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
            String line = measurement;
            line += ",device=";
            line += device.deviceName;
            line += ",unit_id=";
            line += String(device.unitId);
            line += ",register=";
            line += regKv.first;
            if (strlen(regKv.second.unit) > 0) {
                line += ",unit=";
                line += regKv.second.unit;
            }
            line += " value=";
            line += String(regKv.second.value, 4);
            line += " ";
            line += String((uint64_t)regKv.second.timestamp * 1000000000ULL);  // ns
            
            result.push_back(line);
        }
    }
    
    return result;
}
