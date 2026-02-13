#ifndef FEATURE_H
#define FEATURE_H

/**
 * @brief Base class for all firmware features
 *
 * Features implement cooperative non-blocking initialization and periodic
 * work. `setup()` is called once during startup and must not block. `loop()`
 * is invoked frequently from the main thread and must also be non-blocking.
 * Implementations should use state machines and `millis()` timers for any
 * long-running activity.
 */
class Feature {
public:
    virtual ~Feature() = default;
    
    /**
     * @brief Called once during the initialization phase
     *
     * Implement non-blocking initialization here. If the feature cannot be
     * fully initialized synchronously, return and continue setup work from
     * `loop()` or via internal state machines.
     */
    virtual void setup() = 0;
    
    /**
     * @brief Periodic work function invoked from the main loop
     *
     * This must not block. Use `millis()` and small state machines to perform
     * background tasks. Default implementation is a no-op.
     */
    virtual void loop() {}
    
    /**
     * @brief Returns a short name used for logging and diagnostics
     * @return NUL-terminated short name string
     */
    virtual const char* getName() const = 0;
    
    /**
     * @brief Indicates whether the feature is fully initialized and ready
     *
     * Return `true` when the feature can perform its primary responsibilities
     * (e.g., network connected, sensor sampling active). Defaults to `true`.
     */
    virtual bool isReady() const { return true; }
};

#endif // FEATURE_H
