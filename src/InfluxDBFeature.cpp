#include "InfluxDBFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>

// InfluxDB 2.x constructor
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
    , _username("")
    , _password("")
    , _retentionPolicy("")
    , _batchIntervalMs(batchIntervalMs)
    , _batchSize(batchSize)
    , _isV1(false)
    , _ready(false)
    , _connected(false)
    , _enabled(false)
    , _lastUploadTime(0)
    , _stats{0, 0, 0, 0}
{
}

// InfluxDB 1.x constructor (private, used by factory)
InfluxDBFeature::InfluxDBFeature(const char* serverUrl,
                                 const char* database,
                                 const char* username,
                                 const char* password,
                                 const char* retentionPolicy,
                                 uint32_t batchIntervalMs,
                                 size_t batchSize,
                                 bool isV1)
    : _serverUrl(serverUrl)
    , _org("")
    , _bucket(database)  // Reuse bucket field for database name
    , _token("")
    , _username(username)
    , _password(password)
    , _retentionPolicy(retentionPolicy)
    , _batchIntervalMs(batchIntervalMs)
    , _batchSize(batchSize)
    , _isV1(isV1)
    , _ready(false)
    , _connected(false)
    , _enabled(false)
    , _lastUploadTime(0)
    , _stats{0, 0, 0, 0}
{
}

// Factory method for InfluxDB 1.x
InfluxDBFeature InfluxDBFeature::createV1(const char* serverUrl,
                                           const char* database,
                                           const char* username,
                                           const char* password,
                                           const char* retentionPolicy,
                                           uint32_t batchIntervalMs,
                                           size_t batchSize) {
    return InfluxDBFeature(serverUrl, database, username, password, 
                           retentionPolicy, batchIntervalMs, batchSize, true);
}

void InfluxDBFeature::setup() {
    if (_ready) return;
    
    // Check if InfluxDB is configured
    if (_isV1) {
        // V1: needs URL and database
        _enabled = strlen(_serverUrl) > 0 && strlen(_bucket) > 0;
        if (_enabled) {
            LOG_I("InfluxDB 1.x configured: %s (db=%s, user=%s)", 
                  _serverUrl, _bucket, strlen(_username) > 0 ? _username : "(none)");
        }
    } else {
        // V2: needs URL and token
        _enabled = strlen(_serverUrl) > 0 && strlen(_token) > 0;
        if (_enabled) {
            LOG_I("InfluxDB 2.x configured: %s (org=%s, bucket=%s)", 
                  _serverUrl, _org, _bucket);
        }
    }
    
    if (_enabled) {
        LOG_I("  Batch interval: %lu ms, max size: %u", _batchIntervalMs, _batchSize);
    } else {
        LOG_I("InfluxDB disabled (not configured)");
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
    String url;
    
    if (_isV1) {
        // InfluxDB 1.x: /write?db=DATABASE&precision=ns
        url = String(_serverUrl) + "/write?db=" + _bucket + "&precision=ns";
        
        // Add retention policy if specified
        if (strlen(_retentionPolicy) > 0) {
            url += "&rp=" + String(_retentionPolicy);
        }
        
        // Add credentials as query params (alternative to Basic Auth)
        if (strlen(_username) > 0) {
            url += "&u=" + String(_username) + "&p=" + String(_password);
        }
    } else {
        // InfluxDB 2.x: /api/v2/write?org=ORG&bucket=BUCKET&precision=ns
        url = String(_serverUrl) + "/api/v2/write?org=" + _org + 
              "&bucket=" + _bucket + "&precision=ns";
    }
    
    http.begin(url);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.setTimeout(10000);  // 10 second timeout
    
    // Add authentication header
    if (_isV1) {
        // V1: Basic auth if credentials provided
        if (strlen(_username) > 0) {
            http.setAuthorization(_username, _password);
        }
    } else {
        // V2: Bearer token
        http.addHeader("Authorization", String("Token ") + _token);
    }
    
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
