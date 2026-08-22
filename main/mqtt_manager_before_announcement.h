#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    bool auto_enabled;
    bool alarm_active;
    bool rtc_ready;

    uint32_t timetable_count;
    uint32_t event_log_count;
    uint32_t ring_duration_seconds;
    uint32_t uptime_seconds;

    char current_time[32];
    char next_bell[32];
} mqtt_alarm_status_t;

typedef struct
{
    bool connected;

    uint32_t disconnect_count;

    int32_t error_type;
    int32_t socket_errno;
    int32_t tls_error;
    int32_t tls_stack_error;
    int32_t connect_return_code;

    char summary[160];
} mqtt_diagnostics_t;

esp_err_t mqtt_manager_start(void);

esp_err_t mqtt_manager_stop(void);

bool mqtt_manager_is_connected(void);

esp_err_t mqtt_manager_publish_status(
    const mqtt_alarm_status_t *status
);

const char *mqtt_manager_topic_prefix(void);

const char *mqtt_manager_broker_uri(void);

void mqtt_manager_get_diagnostics(
    mqtt_diagnostics_t *diagnostics
);

#endif
