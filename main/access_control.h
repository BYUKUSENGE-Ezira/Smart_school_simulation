#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ACCESS_PIN_MIN_LENGTH 4
#define ACCESS_PIN_MAX_LENGTH 8

#define ACCESS_MAX_FAILED_ATTEMPTS 3
#define ACCESS_LOCKOUT_MS 60000U

typedef enum
{
    ACCESS_RESULT_GRANTED = 0,
    ACCESS_RESULT_DENIED,
    ACCESS_RESULT_LOCKED,
    ACCESS_RESULT_ERROR
} access_result_t;

/*
 * Call after nvs_flash_init().
 *
 * On the first startup, the default PIN is 1234.
 */
esp_err_t access_control_init(void);

/*
 * Verify an entered PIN.
 *
 * now_ms:
 * Current system time in milliseconds.
 *
 * remaining_lockout_ms:
 * Receives the remaining lockout duration.
 */
access_result_t access_control_verify(
    const char *pin,
    uint32_t now_ms,
    uint32_t *remaining_lockout_ms
);

/*
 * Change the administrator PIN.
 *
 * The PIN must contain 4 to 8 numeric digits.
 */
esp_err_t access_control_change_pin(
    const char *new_pin
);

/*
 * Check whether a PIN has a valid format.
 */
bool access_control_pin_is_valid(
    const char *pin
);

/*
 * Return the number of failed attempts made since
 * the last successful login or lockout.
 */
uint8_t access_control_failed_attempts(void);

#endif
