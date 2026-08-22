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

    uint32_t timetable_count;
    uint32_t event_log_count;
    uint32_t ring_duration_seconds;
    uint32_t uptime_seconds;

    char current_time[32];
    char next_bell[32];
    char ip_address[16];
    char wifi_state[20];
} web_status_t;

/*
 * Called by the web server whenever a browser requests
 * current system status.
 */
typedef void (*web_status_provider_t)(
    web_status_t *status
);

/*
 * Start the read-only HTTP server.
 */
esp_err_t web_server_start(
    web_status_provider_t status_provider
);

/*
 * Stop the HTTP server.
 */
esp_err_t web_server_stop(void);

/*
 * Return true while the HTTP server is running.
 */
bool web_server_is_running(void);

#endif

