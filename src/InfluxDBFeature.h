#ifndef INFLUXDB_FEATURE_H
#define INFLUXDB_FEATURE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <vector>
#include "Feature.h"

/**
 * @brief InfluxDB data writer using line protocol over HTTP
 *        Supports both InfluxDB 1.x (user/password) and 2.x (org/bucket/token)
 */
class InfluxDBFeature : public Feature {
public:
    /**
     * @brief Construct InfluxDB 2.x feature (org/bucket/token auth)
     * @param serverUrl InfluxDB server URL (e.g., "http://192.168.1.50:8086")
     * @param org Organization name
     * @param bucket Bucket name
     * @param token API token
     * @param batchIntervalMs Interval between batch uploads (0 = immediate)
     * @param batchSize Max lines to accumulate before auto-upload
     */
    InfluxDBFeature(const char* serverUrl,
                    const char* org,
                    const char* bucket,
                    const char* token,
                    uint32_t batchIntervalMs = 10000,
                    size_t batchSize = 100);
    
    /**
     * @brief Construct InfluxDB 1.x feature (database/user/password auth)
     * @param serverUrl InfluxDB server URL (e.g., "http://192.168.1.50:8086")
     * @param database Database name
     * @param username Username (empty string if no auth)
     * @param password Password (empty string if no auth)
     * @param retentionPolicy Retention policy (empty = default)
     * @param batchIntervalMs Interval between batch uploads (0 = immediate)
     * @param batchSize Max lines to accumulate before auto-upload
     */
    static InfluxDBFeature createV1(const char* serverUrl,
                                     const char* database,
                                     const char* username = "",
                                     const char* password = "",
                                     const char* retentionPolicy = "",
                                     uint32_t batchIntervalMs = 10000,
                                     size_t batchSize = 100);
    
    void setup() override;
    void loop() override;
    const char* getName() const override { return "InfluxDB"; }
    bool isReady() const override { return _ready; }
    
    /**
     * @brief Queue line protocol data for batch upload
     * @param lineProtocol Line protocol formatted string (single or multi-line)
     */
    void queue(const String& lineProtocol);
    
    /**
     * @brief Force immediate upload of queued data
     * @return true if upload successful
     */
    bool upload();
    
    /**
     * @brief Check if connected to InfluxDB (last upload succeeded)
     */
    bool isConnected() const { return _connected; }
    
    /**
     * @brief Get number of pending lines
     */
    size_t pendingCount() const { return _buffer.size(); }
    
    /**
     * @brief Get total bytes pending
     */
    size_t pendingBytes() const;
    
    /**
     * @brief Clear pending buffer
     */
    void clearBuffer() { _buffer.clear(); }
    
    /**
     * @brief Set batch interval
     */
    void setBatchInterval(uint32_t ms) { _batchIntervalMs = ms; }
    
    /**
     * @brief Get upload statistics
     */
    struct Stats {
        uint32_t successCount;
        uint32_t failCount;
        uint32_t totalPointsWritten;
        uint32_t lastUploadMs;
    };
    const Stats& getStats() const { return _stats; }

private:
    bool sendData(const String& data);
    
    // Constructor for internal use (V1 mode)
    InfluxDBFeature(const char* serverUrl,
                    const char* database,
                    const char* username,
                    const char* password,
                    const char* retentionPolicy,
                    uint32_t batchIntervalMs,
                    size_t batchSize,
                    bool isV1);
    
    const char* _serverUrl;
    const char* _org;       // V2: org, V1: unused
    const char* _bucket;    // V2: bucket, V1: database
    const char* _token;     // V2: token, V1: unused
    const char* _username;  // V1 only
    const char* _password;  // V1 only
    const char* _retentionPolicy;  // V1 only
    uint32_t _batchIntervalMs;
    size_t _batchSize;
    bool _isV1;             // true = InfluxDB 1.x, false = 2.x
    
    std::vector<String> _buffer;
    bool _ready;
    bool _connected;
    bool _enabled;
    unsigned long _lastUploadTime;
    
    Stats _stats;
};

#endif // INFLUXDB_FEATURE_H
