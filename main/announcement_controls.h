#ifndef ANNOUNCEMENT_CONTROLS_H
#define ANNOUNCEMENT_CONTROLS_H

#include "esp_err.h"

#define ANNOUNCEMENT_PTT_GPIO       4
#define ANNOUNCEMENT_EMERGENCY_GPIO 5

esp_err_t announcement_controls_init(void);

/*
 * Call continuously from the main application loop.
 */
esp_err_t announcement_controls_poll(void);

#endif
