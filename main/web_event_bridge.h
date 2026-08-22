#ifndef WEB_EVENT_BRIDGE_H
#define WEB_EVENT_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_log.h"

#define WEB_EVENT_MAX_RECORDS 64U

typedef enum
{
    WEB_EVENT_COMMAND_GET_ALL = 1,
    WEB_EVENT_COMMAND_CLEAR
} web_event_command_type_t;

typedef struct
{
    web_event_command_type_t type;

    event_log_record_t *output_records;
    size_t output_capacity;
    size_t *output_count;

    TaskHandle_t requesting_task;
    esp_err_t *result_location;
} web_event_request_t;

/*
 * Create the event command queue.
 */
esp_err_t web_event_bridge_init(void);

/*
 * Functions called by the HTTP server task.
 */
esp_err_t web_event_bridge_get_all(
    event_log_record_t *records,
    size_t capacity,
    size_t *count
);

esp_err_t web_event_bridge_clear(void);

/*
 * Functions used only by the main alarm task.
 */
bool web_event_bridge_receive(
    web_event_request_t *request
);

void web_event_bridge_complete(
    const web_event_request_t *request,
    esp_err_t result
);

#endif
