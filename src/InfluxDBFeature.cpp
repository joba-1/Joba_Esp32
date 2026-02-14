#include "InfluxDBFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

/**
 * @file InfluxDBFeature.cpp
 * @brief Backgrounded InfluxDB line-protocol uploader implementation
 *
 * Implements batching, DNS resolution caching and a background task to
 * upload data without blocking the main loop. The public API is exposed
 * via `InfluxDBFeature` methods defined in the header.
 */

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
    , _stats{}
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
    , _stats{}
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

/**
 * @brief Initialize InfluxDB feature and start background upload task
 *
 * Resolves the configured server (when possible), allocates synchronization
 * primitives and starts a pinned FreeRTOS task to perform HTTP uploads.
 */
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
        
        // Resolve hostname once and build the full URL for uploads
        resolveAndCacheUrl();
        
        // Create mutex for payload handoff
        _payloadMutex = xSemaphoreCreateMutex();
        
        // Create background upload task on core 0 (WiFi core)
        // 4KB stack is enough for HTTPClient + small payload
        xTaskCreatePinnedToCore(
            uploadTaskFunc,
            "influxUp",
            4096,
            this,
            1,          // low priority
            &_uploadTask,
            0           // core 0 (WiFi/network core)
        );
        LOG_I("InfluxDB background upload task started");
    } else {
        LOG_I("InfluxDB disabled (not configured)");
    }
    
    _ready = true;
}

/**
 * @brief Periodic handler: decide whether an upload should be triggered
 *
 * Triggers when batch size or interval conditions are met and WiFi is
 * available. The actual upload is handed off to a background task.
 */
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

/**
 * @brief Queue one or more InfluxDB line-protocol lines for upload
 * @param lineProtocol One or more lines separated by '\n'
 */
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
    
    // Enforce buffer cap — drop oldest lines to stay within limit
    if (_buffer.size() > MAX_BUFFER_LINES) {
        size_t excess = _buffer.size() - MAX_BUFFER_LINES;
        _buffer.erase(_buffer.begin(), _buffer.begin() + excess);
        _stats.droppedLines += excess;
        LOG_W("InfluxDB buffer capped: dropped %u oldest lines (total dropped: %u)",
              excess, _stats.droppedLines);
    }
    
    LOG_V("InfluxDB: queued %u lines, buffer size: %u", 1, _buffer.size());
}

/**
 * @brief Assemble batch payload and hand it to the background task
 * @return true when handoff succeeded (or nothing to do)
 */
bool InfluxDBFeature::upload() {
    if (!_enabled || _buffer.empty()) return true;
    if (WiFi.status() != WL_CONNECTED) {
        LOG_W("InfluxDB upload skipped: WiFi not connected");
        return false;
    }
    
    // Don't queue another upload while the background task is still busy
    if (_uploadInProgress) {
        LOG_V("InfluxDB upload deferred: previous upload still in progress");
        return false;
    }
    
    // Build batch payload
    String payload;
    size_t lineCount = 0;
    // Pre-reserve to avoid reallocation
    size_t totalLen = 0;
    for (const String& line : _buffer) {
        totalLen += line.length() + 1;
    }
    payload.reserve(totalLen);
    
    for (const String& line : _buffer) {
        if (lineCount > 0) payload += "\n";
        payload += line;
        lineCount++;
    }
    
    // Hand off to background task via mutex-protected payload
    if (xSemaphoreTake(_payloadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _pendingPayload = std::move(payload);
        _pendingLineCount = lineCount;
        _uploadInProgress = true;
        xSemaphoreGive(_payloadMutex);
        
        // Wake the background task
        xTaskNotifyGive(_uploadTask);
        
        // Clear buffer immediately — data is now owned by the task
        _buffer.clear();
        _lastUploadTime = millis();
        
        LOG_V("InfluxDB: handed %u lines to background task", lineCount);
        return true;
    } else {
        LOG_W("InfluxDB: mutex timeout, upload deferred");
        return false;
    }
}

/**
 * @brief Perform the HTTP POST to InfluxDB (synchronous; used by background task)
 * @param data Line-protocol payload
 * @return true on success
 */
bool InfluxDBFeature::sendData(const String& data) {
    HTTPClient http;
    
    // Use pre-resolved URL (with IP) to avoid blocking DNS on every upload
    const String& url = _resolvedUrl;
    
    http.begin(url);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    // Keep timeout short — this now runs on a background task so it doesn't
    // block the main loop, but we still don't want to hold the task forever.
    static constexpr int HTTP_TIMEOUT_MS = 3000;
    http.setTimeout(HTTP_TIMEOUT_MS);
    
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
        if (millis() - _lastErrorLog >= _errorLogIntervalMs) {
            LOG_E("InfluxDB error %d: %s", httpCode, response.c_str());
            _lastErrorLog = millis();
        } else {
            LOG_V("InfluxDB error %d (throttled)", httpCode);
        }
        http.end();
        return false;
    } else {
        // Connection error
        if (millis() - _lastErrorLog >= _errorLogIntervalMs) {
            LOG_E("InfluxDB connection error: %s", http.errorToString(httpCode).c_str());
            _lastErrorLog = millis();
        } else {
            LOG_V("InfluxDB connection error (throttled)");
        }
        http.end();
        return false;
    }
}

// ---------- DNS caching ----------

/**
 * @brief Resolve server hostname to IP and construct a numeric upload URL
 *
 * This avoids DNS lookups on every upload by caching an IP-based URL when
 * DNS resolution succeeds. Falls back to the original URL otherwise.
 */
void InfluxDBFeature::resolveAndCacheUrl() {
    // Parse hostname from _serverUrl (format: "http://hostname:port" or "http://ip:port")
    String urlStr(_serverUrl);
    String host;
    int port = 80;
    
    // Strip scheme
    int schemeEnd = urlStr.indexOf("://");
    String rest = (schemeEnd >= 0) ? urlStr.substring(schemeEnd + 3) : urlStr;
    
    // Extract host:port
    int colonPos = rest.indexOf(':');
    int slashPos = rest.indexOf('/');
    if (colonPos > 0 && (slashPos < 0 || colonPos < slashPos)) {
        host = rest.substring(0, colonPos);
        String portStr = rest.substring(colonPos + 1, slashPos > 0 ? slashPos : rest.length());
        port = portStr.toInt();
    } else {
        host = rest.substring(0, slashPos > 0 ? slashPos : rest.length());
    }
    
    // Try to resolve hostname to IP
    IPAddress ip;
    if (WiFi.hostByName(host.c_str(), ip)) {
        // Build URL query parameters
        String params;
        if (_isV1) {
            params = "/write?db=" + String(_bucket) + "&precision=ns";
            if (strlen(_retentionPolicy) > 0) {
                params += "&rp=" + String(_retentionPolicy);
            }
            if (strlen(_username) > 0) {
                params += "&u=" + String(_username) + "&p=" + String(_password);
            }
        } else {
            params = "/api/v2/write?org=" + String(_org) +
                     "&bucket=" + String(_bucket) + "&precision=ns";
        }
        
        _resolvedUrl = "http://" + ip.toString() + ":" + String(port) + params;
        LOG_I("InfluxDB URL resolved: %s -> %s", host.c_str(), _resolvedUrl.c_str());
    } else {
        // Fallback: use original URL (will trigger DNS per request)
        if (_isV1) {
            _resolvedUrl = String(_serverUrl) + "/write?db=" + String(_bucket) + "&precision=ns";
            if (strlen(_retentionPolicy) > 0) {
                _resolvedUrl += "&rp=" + String(_retentionPolicy);
            }
            if (strlen(_username) > 0) {
                _resolvedUrl += "&u=" + String(_username) + "&p=" + String(_password);
            }
        } else {
            _resolvedUrl = String(_serverUrl) + "/api/v2/write?org=" + String(_org) +
                           "&bucket=" + String(_bucket) + "&precision=ns";
        }
        LOG_W("InfluxDB DNS resolve failed for '%s', using original URL", host.c_str());
    }
}

// ---------- Background upload task ----------

/**
 * @brief FreeRTOS task function responsible for performing HTTP uploads
 * @param param Pointer to `InfluxDBFeature` instance
 */
void InfluxDBFeature::uploadTaskFunc(void* param) {
    auto* self = static_cast<InfluxDBFeature*>(param);
    
    for (;;) {
        // Wait for notification from upload() — blocks without consuming CPU
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        
        if (!self->_uploadInProgress) continue;
        
        // Take payload from shared state
        String payload;
        size_t lineCount = 0;
        if (xSemaphoreTake(self->_payloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            payload = std::move(self->_pendingPayload);
            lineCount = self->_pendingLineCount;
            self->_pendingPayload = String();  // release memory
            self->_pendingLineCount = 0;
            xSemaphoreGive(self->_payloadMutex);
        } else {
            self->_uploadInProgress = false;
            continue;
        }
        
        if (payload.length() == 0) {
            self->_uploadInProgress = false;
            continue;
        }
        
        bool ok = false;
        uint32_t totalDurationMs = 0;
        
        for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
            if (attempt > 0) {
                self->_stats.retryCount++;
                LOG_W("InfluxDB retry %d/%d for %u lines", attempt, MAX_RETRIES, lineCount);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS * attempt));
            }
            
            const uint32_t startMs = (uint32_t)millis();
            ok = self->sendData(payload);
            const uint32_t durationMs = (uint32_t)millis() - startMs;
            totalDurationMs += durationMs;
            
            // Update stats (32-bit writes are atomic on ESP32)
            self->_stats.lastUploadDurationMs = durationMs;
            if (durationMs > self->_stats.maxUploadDurationMs) {
                self->_stats.maxUploadDurationMs = durationMs;
            }
            
            if (ok) break;
        }
        
        if (ok) {
            self->_stats.successCount++;
            self->_stats.totalPointsWritten += lineCount;
            self->_stats.lastUploadMs = (uint32_t)millis();
            self->_connected = true;
            LOG_D("InfluxDB bg upload ok: %u lines in %ums", lineCount, totalDurationMs);
        } else {
            self->_stats.failCount++;
            self->_stats.droppedLines += lineCount;
            self->_connected = false;
            LOG_W("InfluxDB bg upload failed after %d retries: %u lines dropped, %ums",
                  MAX_RETRIES, lineCount, totalDurationMs);
        }
        
        self->_uploadInProgress = false;
    }
}

/**
 * @brief Compute approximate number of bytes pending in the in-memory buffer
 */
size_t InfluxDBFeature::pendingBytes() const {
    size_t total = 0;
    for (const String& line : _buffer) {
        total += line.length() + 1;  // +1 for newline
    }
    return total;
}
