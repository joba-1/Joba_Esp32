#ifndef DATA_COLLECTION_H
#define DATA_COLLECTION_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include "StorageFeature.h"
#include "TimeUtils.h"

#ifndef FIRMWARE_NAME
#define FIRMWARE_NAME "ESP32-Firmware"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.0"
#endif

// Field types for schema definition
enum class FieldType { 
    INT8, INT16, INT32, UINT8, UINT16, UINT32, 
    FLOAT, DOUBLE, BOOL, STRING 
};

// InfluxDB field classification
enum class InfluxType { 
    TAG,        // Indexed string field
    FIELD,      // Value field
    TIMESTAMP,  // Unix timestamp (nanoseconds)
    SKIP        // Don't include in line protocol
};

/**
 * @brief Field descriptor for schema definition
 */
struct FieldDescriptor {
    const char* name;       // Field name
    FieldType type;         // Data type
    InfluxType influxType;  // How to handle in InfluxDB
    size_t offset;          // Offset in struct (use offsetof())
    size_t size;            // Size for STRING type
};

// Helper macros for field definition
#define FIELD_INT8(st, f, it)    { #f, FieldType::INT8,   InfluxType::it, offsetof(st, f), sizeof(int8_t) }
#define FIELD_INT16(st, f, it)   { #f, FieldType::INT16,  InfluxType::it, offsetof(st, f), sizeof(int16_t) }
#define FIELD_INT32(st, f, it)   { #f, FieldType::INT32,  InfluxType::it, offsetof(st, f), sizeof(int32_t) }
#define FIELD_UINT8(st, f, it)   { #f, FieldType::UINT8,  InfluxType::it, offsetof(st, f), sizeof(uint8_t) }
#define FIELD_UINT16(st, f, it)  { #f, FieldType::UINT16, InfluxType::it, offsetof(st, f), sizeof(uint16_t) }
#define FIELD_UINT32(st, f, it)  { #f, FieldType::UINT32, InfluxType::it, offsetof(st, f), sizeof(uint32_t) }
#define FIELD_FLOAT(st, f, it)   { #f, FieldType::FLOAT,  InfluxType::it, offsetof(st, f), sizeof(float) }
#define FIELD_DOUBLE(st, f, it)  { #f, FieldType::DOUBLE, InfluxType::it, offsetof(st, f), sizeof(double) }
#define FIELD_BOOL(st, f, it)    { #f, FieldType::BOOL,   InfluxType::it, offsetof(st, f), sizeof(bool) }
#define FIELD_STRING(st, f, it)  { #f, FieldType::STRING, InfluxType::it, offsetof(st, f), sizeof(((st*)0)->f) }

// Forward declaration
class StorageFeature;

/**
 * @brief Ring buffer data collection with JSON/InfluxDB serialization
 * 
 * @tparam T Data structure type
 * @tparam RamCapacity Maximum entries in RAM buffer
 */
template<typename T, size_t RamCapacity = 100>
class DataCollection {
public:
    /**
     * @brief Construct a data collection
     * @param name Collection name (for logging/files)
     * @param schema Array of field descriptors
     * @param fieldCount Number of fields in schema
     * @param influxMeasurement InfluxDB measurement name
     */
    DataCollection(const char* name,
                   const FieldDescriptor* schema,
                   size_t fieldCount,
                   const char* influxMeasurement)
        : _name(name)
        , _schema(schema)
        , _fieldCount(fieldCount)
        , _influxMeasurement(influxMeasurement)
        , _head(0)
        , _tail(0)
        , _count(0)
        , _persistEnabled(false)
        , _storage(nullptr)
        , _filename(nullptr)
        , _persistDelayMs(0)
        , _lastModified(0)
        , _dirty(false)
    {
    }
    
    /**
     * @brief Add a data point to the collection
     * @param data Data to add (timestamp field auto-filled if present)
     */
    void add(const T& data) {
        T entry = data;
        
        // Auto-fill timestamp field if present
        for (size_t i = 0; i < _fieldCount; i++) {
            if (_schema[i].influxType == InfluxType::TIMESTAMP) {
                time_t now;
                time(&now);
                uint32_t* tsPtr = (uint32_t*)((uint8_t*)&entry + _schema[i].offset);
                *tsPtr = (uint32_t)now;
                break;
            }
        }
        
        _buffer[_head] = entry;
        _head = (_head + 1) % RamCapacity;
        
        if (_count < RamCapacity) {
            _count++;
        } else {
            _tail = (_tail + 1) % RamCapacity;
        }
        
        _dirty = true;
        _lastModified = millis();
    }
    
    /**
     * @brief Get number of entries in buffer
     */
    size_t count() const { return _count; }
    
    /**
     * @brief Get capacity of buffer
     */
    size_t capacity() const { return RamCapacity; }
    
    /**
     * @brief Check if buffer is empty
     */
    bool isEmpty() const { return _count == 0; }
    
    /**
     * @brief Check if buffer is full
     */
    bool isFull() const { return _count == RamCapacity; }
    
    /**
     * @brief Get entry by index (0 = oldest)
     */
    const T& get(size_t index) const {
        size_t actualIndex = (_tail + index) % RamCapacity;
        return _buffer[actualIndex];
    }
    
    /**
     * @brief Get most recent entry
     */
    const T& latest() const {
        size_t latestIndex = (_head == 0) ? RamCapacity - 1 : _head - 1;
        return _buffer[latestIndex];
    }
    
    /**
     * @brief Clear all entries
     */
    void clear() {
        _head = 0;
        _tail = 0;
        _count = 0;
        _dirty = true;
        _lastModified = millis();
    }
    
    /**
     * @brief Enable filesystem persistence
     * @param storage Pointer to StorageFeature
     * @param filename File path for storage
     * @param delayMs Delay before writing (0 = immediate)
     */
    void enablePersistence(StorageFeature* storage, const char* filename, uint32_t delayMs = 0) {
        _storage = storage;
        _filename = filename;
        _persistDelayMs = delayMs;
        _persistEnabled = true;
    }
    
    /**
     * @brief Force write to filesystem
     */
    void flush() {
        if (!_persistEnabled || !_storage) return;
        String json = toJson();
        _storage->writeFile(_filename, json);
        _dirty = false;
    }
    
    /**
     * @brief Load data from filesystem
     */
    void load() {
        if (!_persistEnabled || !_storage) return;
        if (!_storage->exists(_filename)) return;
        String json = _storage->readFile(_filename);
        if (json.length() > 0) {
            fromJson(json);
            _dirty = false;
        }
    }
    
    /**
     * @brief Convert all entries to JSON array
     */
    String toJson() const {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        
        for (size_t i = 0; i < _count; i++) {
            JsonObject obj = arr.add<JsonObject>();
            entryToJson(get(i), obj);
        }
        
        String result;
        serializeJson(doc, result);
        return result;
    }
    
    /**
     * @brief Convert single entry to JSON object
     */
    String toJson(size_t index) const {
        if (index >= _count) return "{}";
        
        JsonDocument doc;
        JsonObject obj = doc.to<JsonObject>();
        entryToJson(get(index), obj);
        
        String result;
        serializeJson(doc, result);
        return result;
    }
    
    /**
     * @brief Parse JSON array and add entries
     * @return true if successful
     */
    bool fromJson(const String& json) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, json);
        if (err) return false;
        
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject obj : arr) {
            T entry;
            memset(&entry, 0, sizeof(T));
            jsonToEntry(obj, entry);
            add(entry);
        }
        _dirty = false;  // Just loaded, not dirty
        return true;
    }
    
    /**
     * @brief Convert single entry to InfluxDB line protocol
     */
    String toLineProtocol(size_t index) const {
        if (index >= _count) return "";
        return entryToLineProtocol(get(index));
    }
    
    /**
     * @brief Convert all entries to InfluxDB line protocol (batch)
     */
    String toLineProtocol() const {
        String result;
        for (size_t i = 0; i < _count; i++) {
            if (i > 0) result += "\n";
            result += entryToLineProtocol(get(i));
        }
        return result;
    }
    
    /**
     * @brief Get line protocol for latest entry
     */
    String latestToLineProtocol() const {
        if (_count == 0) return "";
        return entryToLineProtocol(latest());
    }
    
    /**
     * @brief Loop handler for delayed persistence
     */
    void loop() {
        if (!_persistEnabled || !_dirty) return;
        
        if (_persistDelayMs == 0 || (millis() - _lastModified >= _persistDelayMs)) {
            flush();
        }
    }
    
    /**
     * @brief Get collection name
     */
    const char* getName() const { return _name; }
    
    /**
     * @brief Set device ID for InfluxDB tags
     */
    void setDeviceId(const String& deviceId) { _deviceId = deviceId; }

private:
    T _buffer[RamCapacity];
    size_t _head;
    size_t _tail;
    size_t _count;
    
    const char* _name;
    const FieldDescriptor* _schema;
    size_t _fieldCount;
    const char* _influxMeasurement;
    
    // Persistence
    bool _persistEnabled;
    StorageFeature* _storage;
    const char* _filename;
    uint32_t _persistDelayMs;
    uint32_t _lastModified;
    bool _dirty;
    String _deviceId;  // Device ID for InfluxDB tags
    
    void entryToJson(const T& entry, JsonObject& obj) const {
        const uint8_t* ptr = (const uint8_t*)&entry;
        
        for (size_t i = 0; i < _fieldCount; i++) {
            const FieldDescriptor& f = _schema[i];
            const uint8_t* fieldPtr = ptr + f.offset;
            
            switch (f.type) {
                case FieldType::INT8:   obj[f.name] = *(int8_t*)fieldPtr; break;
                case FieldType::INT16:  obj[f.name] = *(int16_t*)fieldPtr; break;
                case FieldType::INT32:  obj[f.name] = *(int32_t*)fieldPtr; break;
                case FieldType::UINT8:  obj[f.name] = *(uint8_t*)fieldPtr; break;
                case FieldType::UINT16: obj[f.name] = *(uint16_t*)fieldPtr; break;
                case FieldType::UINT32: {
                    uint32_t v = *(uint32_t*)fieldPtr;
                    obj[f.name] = v;

                    // If this field is a timestamp, also emit an ISO UTC representation.
                    // Example: "timestamp" -> "timestampIsoUtc".
                    if (f.influxType == InfluxType::TIMESTAMP && TimeUtils::looksLikeUnixSeconds(v)) {
                        String iso = TimeUtils::isoUtcFromUnixSeconds(v);
                        if (iso.length() > 0) {
                            String isoKey = String(f.name) + "IsoUtc";
                            obj[isoKey] = iso;
                        }
                    }
                    break;
                }
                case FieldType::FLOAT:  obj[f.name] = *(float*)fieldPtr; break;
                case FieldType::DOUBLE: obj[f.name] = *(double*)fieldPtr; break;
                case FieldType::BOOL:   obj[f.name] = *(bool*)fieldPtr; break;
                case FieldType::STRING: obj[f.name] = (const char*)fieldPtr; break;
            }
        }
    }
    
    void jsonToEntry(JsonObject& obj, T& entry) const {
        uint8_t* ptr = (uint8_t*)&entry;
        
        for (size_t i = 0; i < _fieldCount; i++) {
            const FieldDescriptor& f = _schema[i];
            uint8_t* fieldPtr = ptr + f.offset;
            
            if (obj[f.name].isNull()) continue;
            
            switch (f.type) {
                case FieldType::INT8:   *(int8_t*)fieldPtr = obj[f.name].as<int8_t>(); break;
                case FieldType::INT16:  *(int16_t*)fieldPtr = obj[f.name].as<int16_t>(); break;
                case FieldType::INT32:  *(int32_t*)fieldPtr = obj[f.name].as<int32_t>(); break;
                case FieldType::UINT8:  *(uint8_t*)fieldPtr = obj[f.name].as<uint8_t>(); break;
                case FieldType::UINT16: *(uint16_t*)fieldPtr = obj[f.name].as<uint16_t>(); break;
                case FieldType::UINT32: *(uint32_t*)fieldPtr = obj[f.name].as<uint32_t>(); break;
                case FieldType::FLOAT:  *(float*)fieldPtr = obj[f.name].as<float>(); break;
                case FieldType::DOUBLE: *(double*)fieldPtr = obj[f.name].as<double>(); break;
                case FieldType::BOOL:   *(bool*)fieldPtr = obj[f.name].as<bool>(); break;
                case FieldType::STRING: 
                    strncpy((char*)fieldPtr, obj[f.name].as<const char*>(), f.size - 1);
                    ((char*)fieldPtr)[f.size - 1] = '\0';
                    break;
            }
        }
    }
    
    String entryToLineProtocol(const T& entry) const {
        const uint8_t* ptr = (const uint8_t*)&entry;
        String line = _influxMeasurement;
        
        // Add global device tags
        line += ",device_id=";
        line += _deviceId.length() > 0 ? escapeTag(_deviceId.c_str()) : "unknown";
        line += ",firmware=";
        line += FIRMWARE_NAME;
        line += ",version=";
        line += FIRMWARE_VERSION;
        
        // Collect tags from data structure
        for (size_t i = 0; i < _fieldCount; i++) {
            const FieldDescriptor& f = _schema[i];
            if (f.influxType != InfluxType::TAG) continue;
            
            const uint8_t* fieldPtr = ptr + f.offset;
            line += ",";
            line += f.name;
            line += "=";
            line += escapeTag((const char*)fieldPtr);
        }
        
        // Collect fields
        line += " ";
        bool firstField = true;
        for (size_t i = 0; i < _fieldCount; i++) {
            const FieldDescriptor& f = _schema[i];
            if (f.influxType != InfluxType::FIELD) continue;
            
            const uint8_t* fieldPtr = ptr + f.offset;
            if (!firstField) line += ",";
            firstField = false;
            line += f.name;
            line += "=";
            
            switch (f.type) {
                case FieldType::INT8:   line += String(*(int8_t*)fieldPtr) + "i"; break;
                case FieldType::INT16:  line += String(*(int16_t*)fieldPtr) + "i"; break;
                case FieldType::INT32:  line += String(*(int32_t*)fieldPtr) + "i"; break;
                case FieldType::UINT8:  line += String(*(uint8_t*)fieldPtr) + "i"; break;
                case FieldType::UINT16: line += String(*(uint16_t*)fieldPtr) + "i"; break;
                case FieldType::UINT32: line += String(*(uint32_t*)fieldPtr) + "i"; break;
                case FieldType::FLOAT:  line += String(*(float*)fieldPtr, 6); break;
                case FieldType::DOUBLE: line += String(*(double*)fieldPtr, 6); break;
                case FieldType::BOOL:   line += *(bool*)fieldPtr ? "true" : "false"; break;
                case FieldType::STRING: 
                    line += "\"";
                    line += escapeString((const char*)fieldPtr);
                    line += "\"";
                    break;
            }
        }
        
        // Add timestamp (nanoseconds)
        for (size_t i = 0; i < _fieldCount; i++) {
            const FieldDescriptor& f = _schema[i];
            if (f.influxType != InfluxType::TIMESTAMP) continue;
            
            const uint8_t* fieldPtr = ptr + f.offset;
            uint32_t ts = *(uint32_t*)fieldPtr;
            line += " ";
            line += String((uint64_t)ts * 1000000000ULL);
            break;
        }
        
        return line;
    }
    
    String escapeTag(const char* str) const {
        String result;
        while (*str) {
            if (*str == ',' || *str == '=' || *str == ' ') {
                result += '\\';
            }
            result += *str++;
        }
        return result;
    }
    
    String escapeString(const char* str) const {
        String result;
        while (*str) {
            if (*str == '"' || *str == '\\') {
                result += '\\';
            }
            result += *str++;
        }
        return result;
    }
};

#endif // DATA_COLLECTION_H
