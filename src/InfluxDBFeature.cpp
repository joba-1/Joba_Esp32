#include "InfluxDBFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>

InfluxDBFeature::InfluxDBFeature(const char* serverUrl,
                                 const char* org,
                                 const char* bucket,
                                 const char* token,
                                 uint32_t batchIntervalMs,
                                 size_t batchSize)
    : _serverUrl(serverUrl)
    , _org(org)
    , _bucket(bucket)
    , _token(token)
    , _batchIntervalMs(batchIntervalMs)
    , _batchSize(batchSize)
    , _ready(false)
    , _connected(false)
    , _enabled(false)
    , _lastUploadTime(0)
    , _stats{0, 0, 0, 0}
{
}

void InfluxDBFeature::setup() {
    if (_ready) return;
    
    // Check if InfluxDB is configured (URL not empty)
    _enabled = strlen(_serverUrl) > 0 && strlen(_token) > 0;
    
    if (_enabled) {
        LOG_I("InfluxDB configured: %s (org=%s, bucket=%s)", _serverUrl, _org, _bucket);
        LOG_I("  Batch interval: %lu ms, max size: %u", _batchIntervalMs, _batchSize);
    } else {
        LOG_I("InfluxDB disabled (no server URL or token configured)");
    }
    
    _ready = true;
}

void InfluxDBFeature::loop() {
    if (!_enabled || !_ready) return;
    if (_buffer.empty()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    
    bool shouldUpload = false;
    
    // Check batch size trigger
    if (_buffer.size() >= _batchSize) {
        shouldUpload = true;
    }
    
    // Check interval trigger
    if (_batchIntervalMs > 0 && (millis() - _lastUploadTime >= _batchIntervalMs)) {
        shouldUpload = true;
    }
    
    if (shouldUpload) {
        upload();
    }
}

void InfluxDBFeature::queue(const String& lineProtocol) {
    if (!_enabled) return;
    if (lineProtocol.length() == 0) return;
    
    // Handle multi-line input (split by newlines)
    int start = 0;
    int end;
    while ((end = lineProtocol.indexOf('\n', start)) != -1) {
        String line = lineProtocol.substring(start, end);
        line.trim();
        if (line.length() > 0) {
            _buffer.push_back(line);
        }
        start = end + 1;
    }
    
    // Handle last line (or single line without newline)
    if (start < (int)lineProtocol.length()) {
        String line = lineProtocol.substring(start);
        line.trim();
        if (line.length() > 0) {
            _buffer.push_back(line);
        }
    }
    
    LOG_V("InfluxDB: queued %u lines, buffer size: %u", 1, _buffer.size());
}

bool InfluxDBFeature::upload() {
    if (!_enabled || _buffer.empty()) return true;
    if (WiFi.status() != WL_CONNECTED) {
        LOG_W("InfluxDB upload skipped: WiFi not connected");
        return false;
    }
    
    // Build batch payload
    String payload;
    size_t lineCount = 0;
    for (const String& line : _buffer) {
        if (lineCount > 0) payload += "\n";
        payload += line;
        lineCount++;
    }
    
    LOG_D("InfluxDB uploading %u lines (%u bytes)", lineCount, payload.length());
    
    if (sendData(payload)) {
        _stats.successCount++;
        _stats.totalPointsWritten += lineCount;
        _stats.lastUploadMs = millis();
        _connected = true;
        _buffer.clear();
        _lastUploadTime = millis();
        LOG_D("InfluxDB upload successful");
        return true;
    } else {
        _stats.failCount++;
        _connected = false;
        LOG_W("InfluxDB upload failed, keeping %u lines in buffer", _buffer.size());
        return false;
    }
}

bool InfluxDBFeature::sendData(const String& data) {
    HTTPClient http;
    
    // Build URL: /api/v2/write?org=ORG&bucket=BUCKET&precision=ns
    String url = String(_serverUrl) + "/api/v2/write?org=" + _org + 
                 "&bucket=" + _bucket + "&precision=ns";
    
    http.begin(url);
    http.addHeader("Authorization", String("Token ") + _token);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.setTimeout(10000);  // 10 second timeout
    
    int httpCode = http.POST(data);
    
    if (httpCode == 204) {
        // Success - InfluxDB returns 204 No Content on successful write
        http.end();
        return true;
    } else if (httpCode > 0) {
        // Got response but not success
        String response = http.getString();
        LOG_E("InfluxDB error %d: %s", httpCode, response.c_str());
        http.end();
        return false;
    } else {
        // Connection error
        LOG_E("InfluxDB connection error: %s", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }
}

size_t InfluxDBFeature::pendingBytes() const {
    size_t total = 0;
    for (const String& line : _buffer) {
        total += line.length() + 1;  // +1 for newline
    }
    return total;
}
