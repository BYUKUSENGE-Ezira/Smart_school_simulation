#ifndef ANNOUNCEMENT_OUTPUT_H
#define ANNOUNCEMENT_OUTPUT_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * Simulation pin representing the PA amplifier enable signal.
 *
 * For real hardware, this pin will control an amplifier,
 * audio switch or relay driver.
 */
#define ANNOUNCEMENT_OUTPUT_GPIO 15

esp_err_t announcement_output_init(void);

/*
 * Synchronize the output with the current announcement state.
 */
esp_err_t announcement_output_sync(void);

bool announcement_output_is_active(void);

#endif
