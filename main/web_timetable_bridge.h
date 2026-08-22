#ifndef WEB_TIMETABLE_BRIDGE_H
#define WEB_TIMETABLE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WEB_TIMETABLE_MAX_ENTRIES 80U

typedef struct
{
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} web_timetable_entry_t;

typedef enum
{
    WEB_TIMETABLE_COMMAND_GET_ALL = 1,
    WEB_TIMETABLE_COMMAND_ADD,
    WEB_TIMETABLE_COMMAND_DELETE,
    WEB_TIMETABLE_COMMAND_RESTORE_DEFAULTS
} web_timetable_command_type_t;

typedef struct
{
    web_timetable_command_type_t type;

    web_timetable_entry_t entry;
    size_t index;

    web_timetable_entry_t *output_entries;
    size_t output_capacity;
    size_t *output_count;

    TaskHandle_t requesting_task;
    esp_err_t *result_location;
} web_timetable_request_t;

/*
 * Create the internal command queue.
 */
esp_err_t web_timetable_bridge_init(void);

/*
 * Functions called by the HTTP server task.
 *
 * They wait until the main alarm task safely completes
 * the requested operation.
 */
esp_err_t web_timetable_bridge_get_all(
    web_timetable_entry_t *entries,
    size_t capacity,
    size_t *count
);

esp_err_t web_timetable_bridge_add(
    const web_timetable_entry_t *entry
);

esp_err_t web_timetable_bridge_delete(
    size_t index
);

esp_err_t web_timetable_bridge_restore_defaults(void);

/*
 * Functions used only by the main alarm task.
 */
bool web_timetable_bridge_receive(
    web_timetable_request_t *request
);

void web_timetable_bridge_complete(
    const web_timetable_request_t *request,
    esp_err_t result
);

#endif
