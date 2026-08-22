#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define EVENT_LOG_MAX_RECORDS 64

typedef enum
{
    EVENT_LOG_SYSTEM_START = 1,

    EVENT_LOG_POWER_ON_RESET,
    EVENT_LOG_SOFTWARE_RESET,
    EVENT_LOG_WATCHDOG_RESET,
    EVENT_LOG_BROWNOUT_RESET,
    EVENT_LOG_PANIC_RESET,
    EVENT_LOG_EXTERNAL_RESET,
    EVENT_LOG_DEEP_SLEEP_WAKE,
    EVENT_LOG_UNKNOWN_RESET,

    EVENT_LOG_SCHEDULED_RING,
    EVENT_LOG_MANUAL_RING,
    EVENT_LOG_BELL_TEST,
    EVENT_LOG_ALARM_STOPPED,

    EVENT_LOG_AUTO_ENABLED,
    EVENT_LOG_AUTO_DISABLED,

    EVENT_LOG_RTC_ERROR,
    EVENT_LOG_RTC_RECOVERED,
    EVENT_LOG_TIME_CHANGED,
    EVENT_LOG_DATE_CHANGED,

    EVENT_LOG_BELL_ADDED,
    EVENT_LOG_BELL_DELETED,
    EVENT_LOG_DEFAULTS_RESTORED,

    EVENT_LOG_DURATION_CHANGED,
    EVENT_LOG_LOGS_CLEARED,

    EVENT_LOG_TIME_FORWARD_JUMP,
    EVENT_LOG_TIME_BACKWARD_JUMP,

    EVENT_LOG_ADMIN_LOGIN_GRANTED,
    EVENT_LOG_ADMIN_LOGIN_FAILED,
    EVENT_LOG_ADMIN_LOCKED,
    EVENT_LOG_ADMIN_PIN_CHANGED,

    EVENT_LOG_ANNOUNCEMENT_LIVE_STARTED,
    EVENT_LOG_ANNOUNCEMENT_LIVE_STOPPED,
    EVENT_LOG_ANNOUNCEMENT_EMERGENCY_STARTED,
    EVENT_LOG_ANNOUNCEMENT_EMERGENCY_STOPPED
} event_log_type_t;

typedef struct
{
    uint16_t year;

    uint8_t month;
    uint8_t date;
    uint8_t weekday;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} event_log_time_t;

typedef struct
{
    uint32_t sequence;

    event_log_type_t type;

    event_log_time_t time;

    int32_t value;
} event_log_record_t;

esp_err_t event_log_init(void);

esp_err_t event_log_add(
    event_log_type_t type,
    const event_log_time_t *time,
    int32_t value
);

size_t event_log_count(void);

bool event_log_get_newest(
    size_t newest_offset,
    event_log_record_t *record
);

esp_err_t event_log_clear(void);

const char *event_log_type_name(
    event_log_type_t type
);

#endif
