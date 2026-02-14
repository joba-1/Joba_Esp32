#ifndef RESET_MANAGER_H
#define RESET_MANAGER_H

#include <Arduino.h>

class ResetManager {
public:
    /**
     * @brief Schedule a device restart
     *
     * Requests a restart after `delayMs` milliseconds. If a restart is
     * already scheduled this returns false and does not modify the pending
     * request.
     *
     * @param delayMs Milliseconds delay until restart
     * @param reason Short reason string recorded for diagnostic purposes
     * @return true when the restart was successfully scheduled
     */
    static bool scheduleRestart(uint32_t delayMs, const char* reason);

    /**
     * @brief Query whether a restart is pending
     * @return true when a restart has been scheduled and not yet executed
     */
    static bool isRestartScheduled();
};

#endif // RESET_MANAGER_H
