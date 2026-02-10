#ifndef MODBUS_DEVICE_FEATURE_H
#define MODBUS_DEVICE_FEATURE_H

#include "Feature.h"
#include "ModbusDevice.h"
#include "LoggingFeature.h"

/**
 * @brief Feature wrapper around ModbusDeviceManager::loop().
 *
 * The manager is created late (after storage is ready), so we accept a
 * pointer that may be null initially.  setup() is a no-op because
 * initialization happens in main.cpp's setup() where storage, device
 * types and mappings are loaded.
 */
class ModbusDeviceFeature : public Feature {
public:
    explicit ModbusDeviceFeature(ModbusDeviceManager*& manager)
        : _manager(manager) {}

    void setup() override {}

    void loop() override {
        if (_manager) {
            _manager->loop();
        }
    }

    const char* getName() const override { return "ModbusDev"; }
    bool isReady() const override { return _manager != nullptr; }

private:
    ModbusDeviceManager*& _manager;   // reference to pointer — follows late init
};

#endif // MODBUS_DEVICE_FEATURE_H
