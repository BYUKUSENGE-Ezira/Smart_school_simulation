#ifndef SCHEDULE_GUARD_H
#define SCHEDULE_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t date;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} schedule_clock_t;

typedef enum
{
    SCHEDULE_GUARD_FIRST_READING = 0,
    SCHEDULE_GUARD_NORMAL,
    SCHEDULE_GUARD_SMALL_DELAY,
    SCHEDULE_GUARD_FORWARD_JUMP,
    SCHEDULE_GUARD_BACKWARD_JUMP
} schedule_guard_status_t;

typedef struct
{
    bool initialized;

    int64_t previous_seconds;
    int64_t current_seconds;

    int32_t elapsed_seconds;

    schedule_guard_status_t status;
} schedule_guard_t;

/*
 * Initialize the guard before using it.
 */
void schedule_guard_init(
    schedule_guard_t *guard
);

/*
 * Process one valid RTC reading.
 *
 * maximum_allowed_delay:
 * Small delays up to this value are accepted.
 *
 * Example:
 * maximum_allowed_delay = 10 seconds.
 */
schedule_guard_status_t schedule_guard_update(
    schedule_guard_t *guard,
    const schedule_clock_t *current_time,
    int32_t maximum_allowed_delay
);

/*
 * Returns true when a bell timestamp lies between
 * the previous RTC reading and the current RTC reading.
 *
 * This is used to catch a bell that was missed by only
 * a few seconds.
 */
bool schedule_guard_crossed_time(
    const schedule_guard_t *guard,
    const schedule_clock_t *bell_time
);

/*
 * Convert a date and time into seconds since 1 January 2000.
 */
int64_t schedule_guard_to_seconds(
    const schedule_clock_t *time
);

#endif
