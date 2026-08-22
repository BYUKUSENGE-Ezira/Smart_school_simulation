#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    bool auto_enabled;
    bool alarm_active;
    bool rtc_ready;
    bool wifi_connected;
    bool mqtt_connected;
    bool nvs_ready;

    uint32_t timetable_count;
    uint32_t event_log_count;
    uint32_t ring_duration_seconds;
    uint32_t uptime_seconds;

    uint32_t nvs_total_entries;
    uint32_t nvs_used_entries;
    uint32_t nvs_free_entries;

    uint32_t app_partition_size_bytes;

    uint32_t timetable_capacity;
    uint32_t event_log_capacity;
    uint32_t mqtt_disconnect_count;

    char current_time[32];
    char next_bell[32];
    char ip_address[16];
    char wifi_state[20];
    char mqtt_topic[128];
    char mqtt_broker[96];
    char mqtt_diagnostic[160];
} web_status_t;

/*
 * Provides the current alarm state to the web server.
 */
typedef void (*web_status_provider_t)(
    web_status_t *status
);

/*
 * Authenticated web-control callbacks.
 */
typedef esp_err_t (*web_auto_setter_t)(
    bool enabled
);

typedef esp_err_t (*web_duration_setter_t)(
    uint32_t seconds
);

typedef struct
{
    web_auto_setter_t set_auto_enabled;
    web_duration_setter_t set_ring_duration;
} web_control_handlers_t;

/*
 * Start the authenticated web server.
 */
esp_err_t web_server_start(
    web_status_provider_t status_provider,
    const web_control_handlers_t *control_handlers
);

/*
 * Stop the HTTP server.
 */
esp_err_t web_server_stop(void);

/*
 * Return true while the server is running.
 */
bool web_server_is_running(void);

#endif
