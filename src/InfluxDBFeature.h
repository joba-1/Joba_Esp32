#ifndef INFLUXDB_FEATURE_H
#define INFLUXDB_FEATURE_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <vector>
#include "Feature.h"

/**
 * @brief InfluxDB 2.x data writer using line protocol over HTTP
 */
class InfluxDBFeature : public Feature {
public:
    /**
     * @brief Construct InfluxDB feature
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
    
    const char* _serverUrl;
    const char* _org;
    const char* _bucket;
    const char* _token;
    uint32_t _batchIntervalMs;
    size_t _batchSize;
    
    std::vector<String> _buffer;
    bool _ready;
    bool _connected;
    bool _enabled;
    unsigned long _lastUploadTime;
    
    Stats _stats;
};

#endif // INFLUXDB_FEATURE_H
