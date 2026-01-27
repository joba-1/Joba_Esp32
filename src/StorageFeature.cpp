#include "StorageFeature.h"
#include "LoggingFeature.h"
#include <ArduinoJson.h>

StorageFeature::StorageFeature(bool formatOnFail)
    : _formatOnFail(formatOnFail)
    , _mounted(false)
{
}

void StorageFeature::setup() {
    if (_mounted) return;
    
    LOG_I("Mounting LittleFS filesystem...");
    
    if (LittleFS.begin(false)) {
        _mounted = true;
        LOG_I("LittleFS mounted. Total: %u bytes, Used: %u bytes", 
              totalBytes(), usedBytes());
    } else if (_formatOnFail) {
        LOG_W("LittleFS mount failed, formatting...");
        if (LittleFS.format()) {
            if (LittleFS.begin(false)) {
                _mounted = true;
                LOG_I("LittleFS formatted and mounted. Total: %u bytes", totalBytes());
            } else {
                LOG_E("LittleFS mount failed after format");
            }
        } else {
            LOG_E("LittleFS format failed");
        }
    } else {
        LOG_E("LittleFS mount failed");
    }
}

bool StorageFeature::writeFile(const char* path, const String& content) {
    if (!_mounted) {
        LOG_E("Storage not mounted");
        return false;
    }
    
    File file = LittleFS.open(path, "w");
    if (!file) {
        LOG_E("Failed to open file for writing: %s", path);
        return false;
    }
    
    size_t written = file.print(content);
    file.close();
    
    if (written != content.length()) {
        LOG_E("Write incomplete: %s (%u/%u bytes)", path, written, content.length());
        return false;
    }
    
    LOG_D("Wrote %u bytes to %s", written, path);
    return true;
}

bool StorageFeature::appendFile(const char* path, const String& content) {
    if (!_mounted) {
        LOG_E("Storage not mounted");
        return false;
    }
    
    File file = LittleFS.open(path, "a");
    if (!file) {
        LOG_E("Failed to open file for appending: %s", path);
        return false;
    }
    
    size_t written = file.print(content);
    file.close();
    
    if (written != content.length()) {
        LOG_E("Append incomplete: %s (%u/%u bytes)", path, written, content.length());
        return false;
    }
    
    LOG_D("Appended %u bytes to %s", written, path);
    return true;
}

String StorageFeature::readFile(const char* path) {
    if (!_mounted) {
        LOG_E("Storage not mounted");
        return "";
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        LOG_W("File not found: %s", path);
        return "";
    }
    
    String content = file.readString();
    file.close();
    
    LOG_D("Read %u bytes from %s", content.length(), path);
    return content;
}

bool StorageFeature::exists(const char* path) {
    if (!_mounted) return false;
    return LittleFS.exists(path);
}

bool StorageFeature::remove(const char* path) {
    if (!_mounted) {
        LOG_E("Storage not mounted");
        return false;
    }
    
    if (!LittleFS.exists(path)) {
        return true;  // Already doesn't exist
    }
    
    if (LittleFS.remove(path)) {
        LOG_D("Removed file: %s", path);
        return true;
    }
    
    LOG_E("Failed to remove file: %s", path);
    return false;
}

bool StorageFeature::mkdir(const char* path) {
    if (!_mounted) {
        LOG_E("Storage not mounted");
        return false;
    }
    
    if (LittleFS.exists(path)) {
        return true;  // Already exists
    }
    
    if (LittleFS.mkdir(path)) {
        LOG_D("Created directory: %s", path);
        return true;
    }
    
    LOG_E("Failed to create directory: %s", path);
    return false;
}

size_t StorageFeature::totalBytes() const {
    if (!_mounted) return 0;
    return LittleFS.totalBytes();
}

size_t StorageFeature::usedBytes() const {
    if (!_mounted) return 0;
    return LittleFS.usedBytes();
}

size_t StorageFeature::freeBytes() const {
    return totalBytes() - usedBytes();
}

String StorageFeature::listDir(const char* path) {
    if (!_mounted) return "[]";
    
    File root = LittleFS.open(path);
    if (!root || !root.isDirectory()) {
        return "[]";
    }
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    File file = root.openNextFile();
    while (file) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = String(file.name());
        obj["size"] = file.size();
        obj["isDir"] = file.isDirectory();
        file = root.openNextFile();
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}
