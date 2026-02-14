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
/**
 * @brief Handle an incoming reset command from MQTT.
 * @param payload The MQTT message payload.
 * @param resetTopic The topic the command was received on.
 * @param mqtt Reference to the `MQTTFeature` instance to publish responses.
 */
void handleResetCommand(const String& payload, const String& resetTopic, MQTTFeature& mqtt);

/**
 * @brief Parse and execute a raw Modbus read command received over MQTT.
 * @param payload JSON or CSV payload describing the read parameters.
 * @param modbusRawReadTopic Topic for raw read commands.
 * @param mqtt MQTTFeature used to send responses or errors.
 * @param modbus ModbusRTUFeature used to perform the raw read.
 */
void handleModbusRawReadCommand(const String& payload, const String& modbusRawReadTopic,
                               MQTTFeature& mqtt, ModbusRTUFeature& modbus);

/**
 * @brief Parse and execute a raw Modbus write command received over MQTT.
 * @param payload Payload describing write address and raw register words.
 * @param modbusRawWriteTopic Topic for raw write commands.
 * @param mqtt MQTTFeature used to send responses or errors.
 * @param modbus ModbusRTUFeature used to perform the raw write.
 */
void handleModbusRawWriteCommand(const String& payload, const String& modbusRawWriteTopic,
                                MQTTFeature& mqtt, ModbusRTUFeature& modbus);

/**
 * @brief Handle a high-level Modbus write command that targets named registers.
 * @param payload JSON payload containing device id, register name and value.
 * @param modbusWriteTopic Topic the write command was received on.
 * @param mqtt MQTTFeature used to send responses or errors.
 * @param modbusDevices Pointer to ModbusDeviceManager for resolving device/registers.
 */
void handleModbusWriteCommand(const String& payload, const String& modbusWriteTopic,
                             MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

/**
 * @brief Handle a high-level Modbus read command that targets named registers.
 * @param payload JSON payload containing device id and register names to read.
 * @param modbusReadTopic Topic the read command was received on.
 * @param mqtt MQTTFeature used to send responses or errors.
 * @param modbusDevices Pointer to ModbusDeviceManager for resolving device/registers.
 */
void handleModbusReadCommand(const String& payload, const String& modbusReadTopic,
                            MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

/**
 * @brief Publish a list of known Modbus devices over MQTT.
 * @param mqtt MQTTFeature used for publishing device list.
 * @param modbusDevices Pointer to ModbusDeviceManager owning device metadata.
 */
void handleModbusListDevicesCommand(MQTTFeature& mqtt, ModbusDeviceManager* modbusDevices);

/**
 * @brief Publish a list of registers for a given device over MQTT.
 * @param payload Payload that should include the device identifier.
 * @param mqtt MQTTFeature used for publishing register lists.
 * @param modbusDevices Pointer to ModbusDeviceManager owning device metadata.
 */
void handleModbusListRegistersCommand(const String& payload, MQTTFeature& mqtt,
                                     ModbusDeviceManager* modbusDevices);

} // namespace MainHelper

#endif // MAIN_HELPER_H