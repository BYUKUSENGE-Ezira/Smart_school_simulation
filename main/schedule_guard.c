#include "schedule_guard.h"

#include <stddef.h>

/* =========================================================
 * Date utility functions
 * ========================================================= */

static bool is_leap_year(
    uint16_t year
)
{
    if (year % 400U == 0U)
    {
        return true;
    }

    if (year % 100U == 0U)
    {
        return false;
    }

    return year % 4U == 0U;
}

static uint8_t days_in_month(
    uint16_t year,
    uint8_t month
)
{
    static const uint8_t month_days[] = {
        0,
        31,
        28,
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31
    };

    if (month < 1U ||
        month > 12U)
    {
        return 0;
    }

    if (month == 2U &&
        is_leap_year(year))
    {
        return 29;
    }

    return month_days[month];
}

static bool clock_is_valid(
    const schedule_clock_t *time
)
{
    if (time == NULL)
    {
        return false;
    }

    if (time->year < 2000U ||
        time->year > 2099U)
    {
        return false;
    }

    if (time->month < 1U ||
        time->month > 12U)
    {
        return false;
    }

    uint8_t maximum_day =
        days_in_month(
            time->year,
            time->month
        );

    if (time->date < 1U ||
        time->date > maximum_day)
    {
        return false;
    }

    if (time->hour > 23U ||
        time->minute > 59U ||
        time->second > 59U)
    {
        return false;
    }

    return true;
}

/* =========================================================
 * Public functions
 * ========================================================= */

int64_t schedule_guard_to_seconds(
    const schedule_clock_t *time
)
{
    if (!clock_is_valid(time))
    {
        return -1;
    }

    int64_t total_days = 0;

    /*
     * Count complete years since 2000.
     */
    for (uint16_t year = 2000U;
         year < time->year;
         year++)
    {
        total_days +=
            is_leap_year(year)
                ? 366
                : 365;
    }

    /*
     * Count complete months in the current year.
     */
    for (uint8_t month = 1U;
         month < time->month;
         month++)
    {
        total_days +=
            days_in_month(
                time->year,
                month
            );
    }

    /*
     * The first day of the month adds zero complete days.
     */
    total_days +=
        time->date - 1U;

    return
        total_days * 86400LL +
        time->hour * 3600LL +
        time->minute * 60LL +
        time->second;
}

void schedule_guard_init(
    schedule_guard_t *guard
)
{
    if (guard == NULL)
    {
        return;
    }

    guard->initialized = false;

    guard->previous_seconds = 0;
    guard->current_seconds = 0;

    guard->elapsed_seconds = 0;

    guard->status =
        SCHEDULE_GUARD_FIRST_READING;
}

schedule_guard_status_t schedule_guard_update(
    schedule_guard_t *guard,
    const schedule_clock_t *current_time,
    int32_t maximum_allowed_delay
)
{
    if (guard == NULL ||
        current_time == NULL)
    {
        return
            SCHEDULE_GUARD_FORWARD_JUMP;
    }

    int64_t new_seconds =
        schedule_guard_to_seconds(
            current_time
        );

    if (new_seconds < 0)
    {
        guard->status =
            SCHEDULE_GUARD_FORWARD_JUMP;

        return guard->status;
    }

    if (!guard->initialized)
    {
        guard->initialized = true;

        guard->previous_seconds =
            new_seconds;

        guard->current_seconds =
            new_seconds;

        guard->elapsed_seconds = 0;

        guard->status =
            SCHEDULE_GUARD_FIRST_READING;

        return guard->status;
    }

    guard->previous_seconds =
        guard->current_seconds;

    guard->current_seconds =
        new_seconds;

    int64_t difference =
        guard->current_seconds -
        guard->previous_seconds;

    if (difference > INT32_MAX)
    {
        guard->elapsed_seconds =
            INT32_MAX;
    }
    else if (difference < INT32_MIN)
    {
        guard->elapsed_seconds =
            INT32_MIN;
    }
    else
    {
        guard->elapsed_seconds =
            (int32_t)difference;
    }

    if (difference < 0)
    {
        guard->status =
            SCHEDULE_GUARD_BACKWARD_JUMP;
    }
    else if (difference == 0 ||
             difference == 1)
    {
        guard->status =
            SCHEDULE_GUARD_NORMAL;
    }
    else if (difference <=
             maximum_allowed_delay)
    {
        guard->status =
            SCHEDULE_GUARD_SMALL_DELAY;
    }
    else
    {
        guard->status =
            SCHEDULE_GUARD_FORWARD_JUMP;
    }

    return guard->status;
}

bool schedule_guard_crossed_time(
    const schedule_guard_t *guard,
    const schedule_clock_t *bell_time
)
{
    if (guard == NULL ||
        bell_time == NULL ||
        !guard->initialized)
    {
        return false;
    }

    if (guard->status !=
            SCHEDULE_GUARD_NORMAL &&
        guard->status !=
            SCHEDULE_GUARD_SMALL_DELAY)
    {
        return false;
    }

    int64_t bell_seconds =
        schedule_guard_to_seconds(
            bell_time
        );

    if (bell_seconds < 0)
    {
        return false;
    }

    return
        bell_seconds >
            guard->previous_seconds &&
        bell_seconds <=
            guard->current_seconds;
}
