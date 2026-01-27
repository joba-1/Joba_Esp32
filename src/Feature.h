#ifndef FEATURE_H
#define FEATURE_H

/**
 * @brief Base class for all features
 * 
 * Non-Blocking Design Principle: Both setup() and loop() methods must be 
 * non-blocking. If an operation cannot complete immediately (e.g., waiting 
 * for network, sensor not ready), the method should return and retry on the 
 * next call. Use state machines or flags to track progress across multiple 
 * invocations.
 */
class Feature {
public:
    virtual ~Feature() = default;
    
    /**
     * @brief Called once during setup phase
     * MUST be non-blocking - if not ready, return and retry on next loop()
     */
    virtual void setup() = 0;
    
    /**
     * @brief Called repeatedly in main loop (optional, default empty)
     * MUST be non-blocking - never use delay() or blocking waits
     */
    virtual void loop() {}
    
    /**
     * @brief Returns feature name for logging/debugging
     */
    virtual const char* getName() const = 0;
    
    /**
     * @brief Returns true when feature is fully initialized and operational
     */
    virtual bool isReady() const { return true; }
};

#endif // FEATURE_H
