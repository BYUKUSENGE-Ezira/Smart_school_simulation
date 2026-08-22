#ifndef SYSTEM_WATCHDOG_H
#define SYSTEM_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Initialize the ESP32 Task Watchdog and subscribe
 * the task that calls this function.
 */
esp_err_t system_watchdog_init(
    uint32_t timeout_ms
);

/*
 * Feed the watchdog.
 *
 * The subscribed task must call this regularly.
 */
esp_err_t system_watchdog_feed(void);

/*
 * Returns true after successful initialization.
 */
bool system_watchdog_is_ready(void);

#endif
