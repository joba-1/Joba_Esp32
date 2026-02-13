#ifndef MODBUS_DEVICE_FEATURE_H
#define MODBUS_DEVICE_FEATURE_H

#include "Feature.h"
#include "ModbusDevice.h"
#include "LoggingFeature.h"

/**
 * @brief Feature wrapper delegating to `ModbusDeviceManager`
 *
 * This `Feature` forwards `loop()` to the referenced `ModbusDeviceManager`.
 * The manager is created after storage is ready, therefore the constructor
 * accepts a reference to a pointer which may be null until initialization.
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
    ModbusDeviceManager*& _manager;   /**< reference to pointer — follows late init */
};

#endif // MODBUS_DEVICE_FEATURE_H
