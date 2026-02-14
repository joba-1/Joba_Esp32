#ifndef SENSOR_COLLECTION_FEATURE_H
#define SENSOR_COLLECTION_FEATURE_H

#include "Feature.h"
#include "DataCollection.h"
#include "DataCollectionMQTT.h"
#include "InfluxDBFeature.h"
#include "MQTTFeature.h"
#include "LEDFeature.h"
#include "LoggingFeature.h"
#include <WiFi.h>

/**
 * @brief Feature wrapper for periodic sensor data collection
 *
 * Manages periodic sampling, buffering and forwarding of sensor readings.
 * This template takes the user-defined measurement struct `T` and a ring
 * buffer capacity. It owns the periodic timer that triggers sampling and
 * calls into `DataCollection::loop()` to perform persistence/housekeeping.
 *
 * The concrete `DataCollection<T, Capacity>` instance is provided by the
 * caller (typically from `main.cpp`) and is not owned by the feature.
 *
 * @tparam T         Sensor data struct type
 * @tparam Capacity  DataCollection ring buffer size
 */
template<typename T, size_t Capacity>
class SensorCollectionFeature : public Feature {
public:
    using CollectFunc = void (*)(T& reading);

    /**
     * @brief Construct a SensorCollectionFeature
     *
     * @param collection     Reference to a `DataCollection<T, Capacity>` buffer
     * @param influxDB       `InfluxDBFeature` used to queue line-protocol
     * @param mqtt           `MQTTFeature` used to publish latest values
     * @param led            `LEDFeature` used to pulse activity indicator
     * @param collectFunc    Function pointer that fills a `T` reading
     * @param collectionName Topic/measurement name (e.g. "sensors")
     * @param intervalMs     Collection interval in milliseconds (default 60s)
     */
    SensorCollectionFeature(DataCollection<T, Capacity>& collection,
                            InfluxDBFeature& influxDB,
                            MQTTFeature& mqtt,
                            LEDFeature& led,
                            CollectFunc collectFunc,
                            const char* collectionName,
                            uint32_t intervalMs = 60000)
        : _collection(collection)
        , _influxDB(influxDB)
        , _mqtt(mqtt)
        , _led(led)
        , _collectFunc(collectFunc)
        , _collectionName(collectionName)
        , _intervalMs(intervalMs)
    {}

    /**
     * @brief No-op setup; the feature is ready immediately
     *
     * Initialization is expected to be handled by the caller; this feature
     * only schedules periodic collection in `loop()`.
     */
    void setup() override {}

    /**
     * @brief Periodic collection tick invoked from main loop
     *
     * Triggers sampling at the configured `_intervalMs`, stores the reading
     * in the ring buffer, queues an InfluxDB line and publishes the latest
     * via MQTT. Also calls `_collection.loop()` to allow persistence or
     * housekeeping.
     */
    void loop() override {
        // Periodic data collection
        if (millis() - _lastCollection >= _intervalMs) {
            _lastCollection = millis();
            collect();
        }

        // DataCollection persistence tick
        _collection.loop();
    }

    const char* getName() const override { return "SensorColl"; }
    bool isReady() const override { return true; }

private:
    /**
     * @brief Perform a single collection cycle
     *
     * This builds a `T` reading via `_collectFunc`, enqueues it to
     * `_collection`, queues the InfluxDB line and publishes via MQTT.
     */
    void collect() {
        T reading;
        memset(&reading, 0, sizeof(reading));

        _collectFunc(reading);
        _collection.add(reading);

        // Queue for InfluxDB
        _influxDB.queue(_collection.latestToLineProtocol());

        // Publish to MQTT
        DataCollectionMQTT::publishLatest(&_mqtt, _collection, _collectionName);

        _led.pulse();
    }

    DataCollection<T, Capacity>& _collection;
    InfluxDBFeature& _influxDB;
    MQTTFeature& _mqtt;
    LEDFeature& _led;
    CollectFunc _collectFunc;
    const char* _collectionName;
    uint32_t _intervalMs;
    uint32_t _lastCollection{0};
};

#endif // SENSOR_COLLECTION_FEATURE_H
