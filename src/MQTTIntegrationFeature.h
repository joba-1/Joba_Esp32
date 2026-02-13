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
/**
 * @brief Glue feature that runs MQTT-related integration tasks
 *
 * Responsibilities:
 * - Publish Home Assistant autodiscovery for configured sensors and Modbus
 * - Subscribe to command topics and maintain subscriptions across reconnects
 * - Periodically publish Modbus device state to MQTT
 *
 * The feature is non-owning: `ModbusDeviceManager` and sensor configs are
 * provided via `configure()` or `setModbusDevices()`.
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

    /**
     * @brief Provide dynamic configuration values
     *
     * This must be called once the runtime values such as `baseTopic`,
     * `deviceId` and the `ModbusDeviceManager` instance are available.
     *
     * @param baseTopic Base MQTT topic used for publishes/subscribes
     * @param deviceId Unique device identifier string
     * @param modbusDevices Pointer to ModbusDeviceManager (non-owning)
     */
    void configure(const char* baseTopic, const char* deviceId,
                   ModbusDeviceManager* modbusDevices) {
        _baseTopic = baseTopic;
        _deviceId = deviceId;
        _modbusDevices = modbusDevices;
    }

    /**
     * @brief No-op setup; work is performed from `loop()` once MQTT is ready
     */
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
            bool ok4 = _mqtt.subscribeToBase("modbus/cmd/raw/write");
            bool ok5 = _mqtt.subscribeToBase("modbus/cmd/write");
            bool ok6 = _mqtt.subscribeToBase("modbus/cmd/read");
            bool ok7 = _mqtt.subscribeToBase("modbus/cmd/list_devices");
            bool ok8 = _mqtt.subscribeToBase("modbus/cmd/list_registers");
            _cmdSubscribed = (ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8);
            LOG_I("MQTT cmd subscribe results: reset=%s restart=%s rawRead=%s rawWrite=%s write=%s read=%s listDevices=%s listRegisters=%s",
                ok1 ? "ok" : "fail", ok2 ? "ok" : "fail", ok3 ? "ok" : "fail", ok4 ? "ok" : "fail",
                ok5 ? "ok" : "fail", ok6 ? "ok" : "fail", ok7 ? "ok" : "fail", ok8 ? "ok" : "fail");
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
