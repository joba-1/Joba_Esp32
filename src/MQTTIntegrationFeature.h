#ifndef MQTT_INTEGRATION_FEATURE_H
#define MQTT_INTEGRATION_FEATURE_H

#include "Feature.h"
#include "MQTTFeature.h"
#include "ModbusDevice.h"
#include "ModbusIntegration.h"
#include "DataCollectionMQTT.h"
#include "DeviceInfo.h"
#include "LoggingFeature.h"

/**
 * @brief Manages all MQTT integration tasks that run in loop():
 *        - Home Assistant autodiscovery (sensors + Modbus)
 *        - MQTT command topic subscriptions (with reconnect re-subscribe)
 *        - Periodic Modbus state publishing
 *
 * Separating these from main.cpp into a Feature gives per-feature CPU
 * timing via CpuMonitor / ResetDiagnostics.
 */
class MQTTIntegrationFeature : public Feature {
public:
    MQTTIntegrationFeature(MQTTFeature& mqtt,
                           const HASensorConfig* sensorHAConfig,
                           size_t sensorHAConfigCount,
                           uint32_t statePublishIntervalMs = 30000)
        : _mqtt(mqtt)
        , _baseTopic(nullptr)
        , _deviceId(nullptr)
        , _sensorHAConfig(sensorHAConfig)
        , _sensorHAConfigCount(sensorHAConfigCount)
        , _modbusDevices(nullptr)
        , _statePublishIntervalMs(statePublishIntervalMs)
    {}

    /// Call after dynamic values (deviceId, baseTopic, modbusDevices) are available
    void configure(const char* baseTopic, const char* deviceId,
                   ModbusDeviceManager* modbusDevices) {
        _baseTopic = baseTopic;
        _deviceId = deviceId;
        _modbusDevices = modbusDevices;
    }

    void setup() override {
        // Nothing to do — all work is deferred to loop() waiting for MQTT
    }

    void loop() override {
        if (!_baseTopic || !_mqtt.isConnected()) {
            _cmdSubscribed = false;
            return;
        }

        // One-shot: sensor HA autodiscovery
        if (!_sensorDiscoveryDone) {
            String deviceName = String(DeviceInfo::getFirmwareName()) + " " + _deviceId;
            DataCollectionMQTT::publishDiscovery(
                &_mqtt,
                "sensors",
                _sensorHAConfig,
                _sensorHAConfigCount,
                deviceName.c_str(),
                _deviceId,
                "joba-1",
                DeviceInfo::getFirmwareName(),
                DeviceInfo::getFirmwareVersion()
            );
            _sensorDiscoveryDone = true;
            LOG_I("Home Assistant autodiscovery published");
        }

        // One-shot (re-armed on disconnect): MQTT command subscriptions
        if (!_cmdSubscribed) {
            bool ok1 = _mqtt.subscribeToBase("cmd/reset");
            bool ok2 = _mqtt.subscribeToBase("cmd/restart");
            bool ok3 = _mqtt.subscribeToBase("modbus/cmd/raw/read");
            _cmdSubscribed = (ok1 && ok2 && ok3);
            LOG_I("MQTT cmd subscribed: %s", _cmdSubscribed ? "yes" : "no");
        }

        // One-shot: Modbus HA autodiscovery
        if (!_modbusDiscoveryDone && _modbusDevices) {
            String modbusTopic = String(_baseTopic) + "/modbus";
            ModbusIntegration::publishDiscovery(
                &_mqtt,
                *_modbusDevices,
                modbusTopic.c_str(),
                "joba-1",
                DeviceInfo::getFirmwareName(),
                DeviceInfo::getFirmwareVersion()
            );
            _modbusDiscoveryDone = true;
            LOG_I("Modbus Home Assistant autodiscovery published");
        }

        // Periodic: Modbus state publish
        if (_modbusDevices &&
            millis() - _lastStatePublish >= _statePublishIntervalMs) {
            _lastStatePublish = millis();
            String modbusTopic = String(_baseTopic) + "/modbus";
            ModbusIntegration::publishAllDeviceStates(&_mqtt, *_modbusDevices,
                                                       modbusTopic.c_str());
        }
    }

    const char* getName() const override { return "MQTTInteg"; }
    bool isReady() const override { return _baseTopic != nullptr; }

    void setModbusDevices(ModbusDeviceManager* mgr) { _modbusDevices = mgr; }

private:
    MQTTFeature& _mqtt;
    const char* _baseTopic;
    const char* _deviceId;
    const HASensorConfig* _sensorHAConfig;
    size_t _sensorHAConfigCount;
    ModbusDeviceManager* _modbusDevices;
    uint32_t _statePublishIntervalMs;

    bool _sensorDiscoveryDone{false};
    bool _modbusDiscoveryDone{false};
    bool _cmdSubscribed{false};
    uint32_t _lastStatePublish{0};
};

#endif // MQTT_INTEGRATION_FEATURE_H
