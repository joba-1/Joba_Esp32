#ifndef RESET_MANAGER_H
#define RESET_MANAGER_H

#include <Arduino.h>

class ResetManager {
public:
    // Schedules an ESP restart after delayMs. Returns false if a restart is already scheduled.
    static bool scheduleRestart(uint32_t delayMs, const char* reason);

    // Returns true if a restart has been scheduled but not executed yet.
    static bool isRestartScheduled();
};

#endif // RESET_MANAGER_H
