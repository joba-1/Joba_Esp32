#ifndef STORAGE_FEATURE_H
#define STORAGE_FEATURE_H

#include <Arduino.h>
#include <LittleFS.h>
#include "Feature.h"

/**
 * @brief LittleFS filesystem wrapper feature
 */
class StorageFeature : public Feature {
public:
    /**
     * @brief Construct storage feature
     * @param formatOnFail Format filesystem if mount fails
     */
    StorageFeature(bool formatOnFail = true);
    
    void setup() override;
    const char* getName() const override { return "Storage"; }
    bool isReady() const override { return _mounted; }
    
    /**
     * @brief Write content to file (overwrites if exists)
     * @param path File path (must start with /)
     * @param content Content to write
     * @return true if successful
     */
    bool writeFile(const char* path, const String& content);
    
    /**
     * @brief Append content to file
     * @param path File path
     * @param content Content to append
     * @return true if successful
     */
    bool appendFile(const char* path, const String& content);
    
    /**
     * @brief Read entire file content
     * @param path File path
     * @return File content, or empty string on error
     */
    String readFile(const char* path);
    
    /**
     * @brief Check if file exists
     */
    bool exists(const char* path);
    
    /**
     * @brief Remove a file
     * @return true if successful or file didn't exist
     */
    bool remove(const char* path);
    
    /**
     * @brief Create directory (and parents)
     */
    bool mkdir(const char* path);
    
    /**
     * @brief Get total filesystem size in bytes
     */
    size_t totalBytes() const;
    
    /**
     * @brief Get used filesystem size in bytes
     */
    size_t usedBytes() const;
    
    /**
     * @brief Get free filesystem size in bytes
     */
    size_t freeBytes() const;
    
    /**
     * @brief List files in directory
     * @param path Directory path
     * @return JSON array of filenames
     */
    String listDir(const char* path);

private:
    bool _formatOnFail;
    bool _mounted;
};

#endif // STORAGE_FEATURE_H
