#ifndef ANNOUNCEMENT_MANAGER_H
#define ANNOUNCEMENT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    ANNOUNCEMENT_STATE_IDLE = 0,
    ANNOUNCEMENT_STATE_LIVE,
    ANNOUNCEMENT_STATE_EMERGENCY
} announcement_state_t;

typedef struct
{
    announcement_state_t state;

    bool live_requested;
    bool emergency_active;
    bool bell_blocked;

    uint32_t transition_count;
} announcement_status_t;

/*
 * Initialize announcement state management.
 */
esp_err_t announcement_manager_init(void);

/*
 * Director starts holding the push-to-talk control.
 *
 * This starts a live announcement unless emergency mode
 * currently has higher priority.
 */
esp_err_t announcement_start_live(void);

/*
 * Director releases the push-to-talk control.
 */
esp_err_t announcement_stop_live(void);

/*
 * Start emergency announcement mode.
 *
 * Emergency mode has the highest priority.
 */
esp_err_t announcement_start_emergency(void);

/*
 * Stop emergency mode.
 *
 * If live push-to-talk is still requested, the state returns
 * to LIVE. Otherwise, it returns to IDLE.
 */
esp_err_t announcement_stop_emergency(void);

/*
 * Copy the current announcement status safely.
 */
esp_err_t announcement_get_status(
    announcement_status_t *status
);

/*
 * Return true while either a live or emergency announcement
 * is active.
 */
bool announcement_is_active(void);

/*
 * Return true while school-bell output must be blocked.
 */
bool announcement_blocks_bell(void);

/*
 * Return a readable state name.
 */
const char *announcement_state_name(
    announcement_state_t state
);

#endif
