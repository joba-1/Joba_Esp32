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
 * @brief Feature wrapper for periodic sensor data collection.
 *
 * Owns the periodic timer + DataCollection::loop() persistence call that
 * previously lived as inline code in main.cpp's loop().  The actual
 * DataCollection instance and struct definition remain in main.cpp;
 * this feature just references them.
 *
 * @tparam T         Sensor data struct type
 * @tparam Capacity  DataCollection ring buffer size
 */
template<typename T, size_t Capacity>
class SensorCollectionFeature : public Feature {
public:
    using CollectFunc = void (*)(T& reading);

    /**
     * @param collection     The DataCollection instance to manage
     * @param influxDB       InfluxDB feature for queuing line protocol
     * @param mqtt           MQTT feature for publishing latest values
     * @param led            LED feature for activity pulse
     * @param collectFunc    Function that fills a T reading with sensor values
     * @param collectionName Topic/measurement name (e.g. "sensors")
     * @param intervalMs     Collection interval (default 60s)
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

    void setup() override {}

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
