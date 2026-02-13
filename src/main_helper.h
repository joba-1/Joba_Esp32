#ifndef MAIN_HELPER_H
#define MAIN_HELPER_H

#include <ArduinoJson.h>
#include <vector>
#include "ModbusDevice.h"
#include "MQTTFeature.h"
#include "ModbusRTUFeature.h"

// Forward declarations
class ModbusDeviceManager;
class MQTTFeature;
class ModbusRTUFeature;

namespace MainHelper {

// Helper functions for MQTT command processing
void handleResetCommand(const String& payload, const String& resetTopic, MQTTFeature& mqtt);

void handleModbusRawReadCommand(const String& payload, const String& modbusRawReadTopic,
                               MQTTFeature& mqtt, ModbusRTUFeature& modbus);

void handleModbusRawWriteCommand(const String& payload, const String& modbusRawWriteTopic,
                                MQTTFeature& mqtt, ModbusRTUFeature& modbus);

void handleModbusWriteCommand(const String& payload, const String& modbusWriteTopic,
                             MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

void handleModbusReadCommand(const String& payload, const String& modbusReadTopic,
                            MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

void handleModbusListDevicesCommand(MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

void handleModbusListRegistersCommand(const String& payload, MQTTFeature& mqtt,
                                     ModbusDeviceManager* modbusDevices);

} // namespace MainHelper

#endif // MAIN_HELPER_H