#ifndef MODBUS_DEVICE_H
#define MODBUS_DEVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ModbusRTUFeature.h"
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
    // Seconds timestamp intended for API output:
    // - Unix epoch seconds when available, otherwise uptime seconds.
    uint32_t timestamp;

    // Monotonic timestamp for scheduling/polling (millis()).
    uint32_t updatedAtMs;

    // Unix epoch seconds at capture time (0 if time not synced/available).
    uint32_t unixTimestamp;
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
    String deviceName;          // Friendly name from mapping
    String deviceTypeName;
    const ModbusDeviceType* deviceType;
    std::map<String, ModbusRegisterValue> currentValues;  // name -> value
    std::map<uint16_t, ModbusRegisterValue> unknownU16;    // address -> value (only when no JSON reg matches)
    unsigned long lastPollTime;
    uint32_t successCount;
    uint32_t errorCount;

    struct ModbusPollBatch {
        uint8_t functionCode;
        uint16_t startAddress;
        uint16_t quantity;          // number of registers
        uint32_t pollIntervalMs;
        uint32_t lastPollMs;        // millis() when last queued successfully
        uint32_t lastAttemptMs;     // millis() when we last attempted to queue
    };

    // Precomputed poll plan: contiguous register windows per (functionCode, pollIntervalMs)
    std::vector<ModbusPollBatch> pollBatches;
};

/**
 * @brief High-level Modbus device manager
 * 
 * Manages device type definitions and unit ID mappings,
 * provides automatic polling and data conversion.
 */
class ModbusDeviceManager {
public:
    class ScopedLock {
    public:
        explicit ScopedLock(SemaphoreHandle_t mutex);
        ~ScopedLock();

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
        ScopedLock(ScopedLock&& other) noexcept;
        ScopedLock& operator=(ScopedLock&& other) noexcept;

    private:
        SemaphoreHandle_t _mutex;
    };

    ScopedLock scopedLock() const;

    /**
     * @brief Callback type for value changes
     * @param unitId Device unit ID
     * @param deviceName Device name from mapping
     * @param registerName Register name
     * @param value Converted value
     * @param unit Unit string
     */
    using ValueChangeCallback = std::function<void(uint8_t unitId, const char* deviceName,
                                                    const char* registerName, float value,
                                                    const char* unit)>;
    
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
     * @brief Stream all current values for a device as JSON
     *
     * Avoids building a potentially large intermediate String.
     */
    void writeDeviceValuesJson(uint8_t unitId, Print& out) const;

    /**
     * @brief Stream a lightweight device summary as JSON
     *
     * Intended for debugging/health checks; avoids iterating all values.
     */
    void writeDeviceMetaJson(uint8_t unitId, Print& out) const;
    
    /**
     * @brief Process automatic polling (call from loop)
     */
    void loop();
    
    /**
     * @brief Get list of loaded device types
     */
    std::vector<String> getDeviceTypeNames() const;
    
    /**
     * @brief Register callback for value changes
     * Called whenever a register value is successfully updated
     */
    void onValueChange(ValueChangeCallback callback) { _valueChangeCallback = callback; }
    
    /**
     * @brief Generate InfluxDB line protocol for a device's current values
     * @param unitId Device unit ID
     * @param measurement InfluxDB measurement name
     * @return Line protocol string (empty if device not found)
     */
    String toLineProtocol(uint8_t unitId, const char* measurement = "modbus") const;
    
    /**
     * @brief Generate InfluxDB line protocol for all devices
     * @param measurement InfluxDB measurement name
     * @return Vector of line protocol strings (one per register value)
     */
    std::vector<String> allToLineProtocol(const char* measurement = "modbus") const;

private:
    void handleObservedFrame(const ModbusFrame& frame, bool isRequest);
    void tryUpdateFromPassiveResponse(ModbusDeviceInstance& device,
                                     const ModbusFrame& request,
                                     const ModbusFrame& response);

    void rebuildPollBatches(ModbusDeviceInstance& device);
    void applyReadResponseToDevice(ModbusDeviceInstance& device,
                                   uint8_t functionCode,
                                   uint32_t pollIntervalMs,
                                   uint16_t startAddress,
                                   const ModbusFrame& response);

    float convertRawToValue(const ModbusRegisterDef& def, const uint16_t* rawData) const;
    std::vector<uint16_t> convertValueToRaw(const ModbusRegisterDef& def, float value) const;
    const ModbusRegisterDef* findRegister(const ModbusDeviceType* type, const char* name) const;
    ModbusDataType parseDataType(const char* str) const;
    void notifyValueChange(uint8_t unitId, const char* registerName, float value, const char* unit);
    
    ModbusRTUFeature& _modbus;
    StorageFeature& _storage;
    
    std::map<String, ModbusDeviceType> _deviceTypes;
    std::map<uint8_t, ModbusDeviceInstance> _devices;
    
    // Polling state
    uint8_t _currentPollUnit;
    size_t _currentPollIndex;
    
    // Value change callback
    ValueChangeCallback _valueChangeCallback;

    // Passive bus tracking: last request per unit, used to infer start register for responses.
    std::map<uint8_t, ModbusFrame> _lastSeenRequests;

    mutable SemaphoreHandle_t _mutex;
};

#endif // MODBUS_DEVICE_H
