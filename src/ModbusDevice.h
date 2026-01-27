#ifndef MODBUS_DEVICE_H
#define MODBUS_DEVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include "ModbusRTU.h"
#include "StorageFeature.h"
#include "DataCollection.h"
#include "LoggingFeature.h"

/**
 * @brief Data types for Modbus register interpretation
 */
enum class ModbusDataType : uint8_t {
    UINT16 = 0,     // Unsigned 16-bit
    INT16,          // Signed 16-bit
    UINT32_BE,      // Unsigned 32-bit big-endian (AB CD)
    UINT32_LE,      // Unsigned 32-bit little-endian (CD AB)
    INT32_BE,       // Signed 32-bit big-endian
    INT32_LE,       // Signed 32-bit little-endian
    FLOAT32_BE,     // IEEE 754 float big-endian
    FLOAT32_LE,     // IEEE 754 float little-endian
    BOOL,           // Boolean (0/1)
    STRING          // ASCII string
};

/**
 * @brief Single register definition
 */
struct ModbusRegisterDef {
    char name[32];              // Register name
    uint16_t address;           // Start register address
    uint16_t length;            // Number of registers
    uint8_t functionCode;       // Function code (3 or 4 typically)
    ModbusDataType dataType;    // Data type
    float conversionFactor;     // Multiply raw value by this
    float offset;               // Add this after multiplication
    char unit[16];              // Unit string (°C, kW, etc.)
    uint32_t pollIntervalMs;    // How often to poll (0 = on-demand)
};

/**
 * @brief Device type definition (loaded from file)
 */
struct ModbusDeviceType {
    char name[32];                          // Device type name
    std::vector<ModbusRegisterDef> registers;
};

/**
 * @brief Register value with converted data
 */
struct ModbusRegisterValue {
    uint32_t timestamp;
    char name[32];
    float value;
    char unit[16];
    bool valid;
};

/**
 * @brief Schema for ModbusRegisterValue data collection
 */
const FieldDescriptor ModbusRegisterValueSchema[] = {
    FIELD_UINT32(ModbusRegisterValue, timestamp, TIMESTAMP),
    FIELD_STRING(ModbusRegisterValue, name, TAG),
    FIELD_FLOAT(ModbusRegisterValue, value, FIELD),
    FIELD_STRING(ModbusRegisterValue, unit, TAG),
    FIELD_BOOL(ModbusRegisterValue, valid, FIELD),
};

/**
 * @brief Active device instance with data collection
 */
struct ModbusDeviceInstance {
    uint8_t unitId;
    String deviceTypeName;
    const ModbusDeviceType* deviceType;
    std::map<String, ModbusRegisterValue> currentValues;  // name -> value
    unsigned long lastPollTime;
    uint32_t successCount;
    uint32_t errorCount;
};

/**
 * @brief High-level Modbus device manager
 * 
 * Manages device type definitions and unit ID mappings,
 * provides automatic polling and data conversion.
 */
class ModbusDeviceManager {
public:
    /**
     * @brief Construct device manager
     * @param modbus Reference to ModbusRTU feature
     * @param storage Reference to storage feature for loading definitions
     */
    ModbusDeviceManager(ModbusRTUFeature& modbus, StorageFeature& storage);
    
    /**
     * @brief Load device type definitions from a JSON file
     * @param path Path to device type definition file
     * @return true if loaded successfully
     * 
     * Expected format:
     * {
     *   "name": "SDM120",
     *   "registers": [
     *     {
     *       "name": "Voltage",
     *       "address": 0,
     *       "length": 2,
     *       "functionCode": 4,
     *       "dataType": "float32_be",
     *       "factor": 1.0,
     *       "offset": 0,
     *       "unit": "V",
     *       "pollInterval": 5000
     *     }
     *   ]
     * }
     */
    bool loadDeviceType(const char* path);
    
    /**
     * @brief Load device mappings from a JSON file
     * @param path Path to mappings file
     * @return true if loaded successfully
     * 
     * Expected format:
     * {
     *   "devices": [
     *     {"unitId": 1, "type": "SDM120", "name": "Main Meter"},
     *     {"unitId": 2, "type": "SDM120", "name": "Solar Meter"}
     *   ]
     * }
     */
    bool loadDeviceMappings(const char* path);
    
    /**
     * @brief Load all device types from a directory
     */
    bool loadAllDeviceTypes(const char* directory = "/modbus/devices");
    
    /**
     * @brief Get a device type by name
     */
    const ModbusDeviceType* getDeviceType(const char* name) const;
    
    /**
     * @brief Get a device instance by unit ID
     */
    ModbusDeviceInstance* getDevice(uint8_t unitId);
    
    /**
     * @brief Get all device instances
     */
    const std::map<uint8_t, ModbusDeviceInstance>& getDevices() const { return _devices; }
    
    /**
     * @brief Read a specific register from a device
     * @param unitId Device unit ID
     * @param registerName Register name from definition
     * @param callback Called with result
     */
    bool readRegister(uint8_t unitId, const char* registerName,
                      std::function<void(bool success, float value)> callback = nullptr);
    
    /**
     * @brief Read all registers for a device
     */
    bool readAllRegisters(uint8_t unitId,
                          std::function<void(bool success)> callback = nullptr);
    
    /**
     * @brief Write a value to a register
     * @param unitId Device unit ID
     * @param registerName Register name from definition
     * @param value Value to write (will be converted based on type)
     */
    bool writeRegister(uint8_t unitId, const char* registerName, float value,
                       std::function<void(bool success)> callback = nullptr);
    
    /**
     * @brief Write raw register value
     */
    bool writeRawRegister(uint8_t unitId, uint8_t functionCode,
                          uint16_t address, uint16_t value,
                          std::function<void(bool success)> callback = nullptr);
    
    /**
     * @brief Write multiple raw registers
     */
    bool writeRawRegisters(uint8_t unitId, uint16_t startAddress,
                           const std::vector<uint16_t>& values,
                           std::function<void(bool success)> callback = nullptr);
    
    /**
     * @brief Get current value of a register (from cache)
     */
    bool getValue(uint8_t unitId, const char* registerName, float& value) const;
    
    /**
     * @brief Get all current values for a device as JSON
     */
    String getDeviceValuesJson(uint8_t unitId) const;
    
    /**
     * @brief Process automatic polling (call from loop)
     */
    void loop();
    
    /**
     * @brief Get list of loaded device types
     */
    std::vector<String> getDeviceTypeNames() const;

private:
    float convertRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData) const;
    std::vector<uint16_t> convertValueToRaw(const ModbusRegisterDef& def, float value) const;
    const ModbusRegisterDef* findRegister(const ModbusDeviceType* type, const char* name) const;
    ModbusDataType parseDataType(const char* str) const;
    
    ModbusRTUFeature& _modbus;
    StorageFeature& _storage;
    
    std::map<String, ModbusDeviceType> _deviceTypes;
    std::map<uint8_t, ModbusDeviceInstance> _devices;
    
    // Polling state
    uint8_t _currentPollUnit;
    size_t _currentPollIndex;
};

#endif // MODBUS_DEVICE_H
