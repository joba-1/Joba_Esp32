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

    // Ensure parent directories exist
    String p = String(path);
    int lastSlash = p.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = p.substring(0, lastSlash);
        if (!LittleFS.exists(parent.c_str())) {
            // Try to create parent directories recursively
            if (!mkdir(parent.c_str())) {
                LOG_E("Failed to create parent directory: %s", parent.c_str());
                return false;
            }
        }
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

    // Ensure parent directories exist
    String p = String(path);
    int lastSlash = p.lastIndexOf('/');
    if (lastSlash > 0) {
        String parent = p.substring(0, lastSlash);
        if (!LittleFS.exists(parent.c_str())) {
            if (!mkdir(parent.c_str())) {
                LOG_E("Failed to create parent directory: %s", parent.c_str());
                return false;
            }
        }
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

    String p = String(path);
    if (p.endsWith("/")) p.remove(p.length() - 1);
    if (p.length() == 0) return true;

    // If already exists, nothing to do
    if (LittleFS.exists(p.c_str())) return true;

    // Create path recursively: from root to full path
    String accum = "";
    int start = 0;
    // Ensure leading '/' is preserved
    if (p.charAt(0) == '/') {
        accum = "/";
        start = 1;
    }

    while (start <= (int)p.length()) {
        int next = p.indexOf('/', start);
        String segment;
        if (next == -1) {
            segment = p.substring(start);
            start = p.length() + 1; // will exit loop
        } else {
            segment = p.substring(start, next);
            start = next + 1;
        }
        if (segment.length() == 0) continue;
        if (accum.endsWith("/") && accum.length() > 1) {
            accum += segment;
        } else if (accum == "/") {
            accum += segment;
        } else if (accum.length() == 0) {
            accum = segment;
        } else {
            accum += "/" + segment;
        }

        if (!LittleFS.exists(accum.c_str())) {
            if (!LittleFS.mkdir(accum.c_str())) {
                LOG_E("Failed to create directory: %s", accum.c_str());
                return false;
            }
            LOG_D("Created directory: %s", accum.c_str());
        }
    }

    return true;
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
