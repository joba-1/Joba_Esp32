#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <Arduino.h>
#include <WiFi.h>

/**
 * @brief Device identification helper
 * 
 * Generates unique device identifiers based on firmware name and either:
 * - Manual instance number (DEVICE_INSTANCE > 0)
 * - Last 3 bytes of MAC address (DEVICE_INSTANCE == 0)
 */
class DeviceInfo {
public:
    /**
     * @brief Get unique device ID (e.g., "MyProject-A1B2C3" or "MyProject-1")
     */
    static String getDeviceId() {
        static String deviceId = generateDeviceId();
        return deviceId;
    }
    
    /**
     * @brief Get hostname-safe device ID (lowercase, no spaces)
     * @return Hostname like "myproject-a1b2c3"
     */
    static String getHostname() {
        static String hostname = []() {
            String h = getDeviceId();
            h.toLowerCase();
            h.replace(" ", "-");
            h.replace("_", "-");
            return h;
        }();
        return hostname;
    }
    
    /**
     * @brief Get firmware name
     */
    static const char* getFirmwareName() {
        return FIRMWARE_NAME;
    }
    
    /**
     * @brief Get firmware version
     */
    static const char* getFirmwareVersion() {
        return FIRMWARE_VERSION;
    }
    
    /**
     * @brief Get device instance number (0 if using MAC-based ID)
     */
    static int getDeviceInstance() {
        return DEVICE_INSTANCE;
    }
    
    /**
     * @brief Get MAC address as hex string (last 3 bytes)
     */
    static String getMacSuffix() {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        // If MAC is all zeros the WiFi stack may not have initialized yet.
        // Use lower 24 bits of efuse MAC as a deterministic fallback.
        if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 0) {
            uint64_t efuse = ESP.getEfuseMac();
            char buf[7];
            snprintf(buf, sizeof(buf), "%06llX", (unsigned long long)(efuse & 0xFFFFFFULL));
            return String(buf);
        }
        char macStr[9];
        snprintf(macStr, sizeof(macStr), "%02X%02X%02X", 
                 mac[3], mac[4], mac[5]);
        return String(macStr);
    }
    
    /**
     * @brief Get default password (firmware name + device ID)
     * @param overridePassword Override password from config (empty = use generated)
     */
    static String getDefaultPassword(const char* overridePassword = "") {
        if (overridePassword && strlen(overridePassword) > 0) {
            return String(overridePassword);
        }
        // Generate password from firmware name + MAC suffix
        String pwd = String(FIRMWARE_NAME);
        pwd += "-";
        pwd += getMacSuffix();
        return pwd;
    }

private:
    static String generateDeviceId() {
        String id = String(FIRMWARE_NAME);
        
#if DEVICE_INSTANCE > 0
        // Use manual instance number
        id += "-";
        id += String(DEVICE_INSTANCE);
#else
        // Use last 3 bytes of MAC address
        id += "-";
        id += getMacSuffix();
#endif
        
        return id;
    }
};

#endif // DEVICE_INFO_H
