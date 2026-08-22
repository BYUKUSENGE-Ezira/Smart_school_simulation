#include "web_timetable_bridge.h"

#include <string.h>

#include "freertos/queue.h"

#include "esp_log.h"

/* =========================================================
 * Configuration
 * ========================================================= */

#define WEB_TIMETABLE_QUEUE_LENGTH 4U

static const char *TAG =
    "WEB_TIMETABLE";

/* =========================================================
 * Static queue storage
 * ========================================================= */

static StaticQueue_t
    command_queue_control;

static uint8_t command_queue_storage[
    WEB_TIMETABLE_QUEUE_LENGTH *
    sizeof(web_timetable_request_t)
];

static QueueHandle_t command_queue =
    NULL;

static bool bridge_ready = false;

/* =========================================================
 * Internal request helper
 * ========================================================= */

static esp_err_t submit_request(
    web_timetable_request_t *request
)
{
    if (!bridge_ready ||
        command_queue == NULL ||
        request == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t operation_result =
        ESP_FAIL;

    request->requesting_task =
        xTaskGetCurrentTaskHandle();

    request->result_location =
        &operation_result;

    /*
     * Remove an old notification before submitting a new
     * synchronous command.
     */
    ulTaskNotifyTake(
        pdTRUE,
        0
    );

    BaseType_t queued =
        xQueueSend(
            command_queue,
            request,
            pdMS_TO_TICKS(1000U)
        );

    if (queued != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Once queued, keep this task and its local result
     * variable alive until the main alarm task completes it.
     *
     * The system watchdog will restart the controller if the
     * main task becomes permanently unresponsive.
     */
    ulTaskNotifyTake(
        pdTRUE,
        portMAX_DELAY
    );

    return operation_result;
}

/* =========================================================
 * Public initialization
 * ========================================================= */

esp_err_t web_timetable_bridge_init(void)
{
    if (bridge_ready)
    {
        return ESP_OK;
    }

    command_queue =
        xQueueCreateStatic(
            WEB_TIMETABLE_QUEUE_LENGTH,
            sizeof(web_timetable_request_t),
            command_queue_storage,
            &command_queue_control
        );

    if (command_queue == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create timetable command queue"
        );

        return ESP_ERR_NO_MEM;
    }

    bridge_ready = true;

    ESP_LOGI(
        TAG,
        "Timetable command bridge initialized"
    );

    return ESP_OK;
}

/* =========================================================
 * HTTP-task request functions
 * ========================================================= */

esp_err_t web_timetable_bridge_get_all(
    web_timetable_entry_t *entries,
    size_t capacity,
    size_t *count
)
{
    if (entries == NULL ||
        count == NULL ||
        capacity == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0U;

    web_timetable_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_TIMETABLE_COMMAND_GET_ALL;

    request.output_entries =
        entries;

    request.output_capacity =
        capacity;

    request.output_count =
        count;

    return submit_request(
        &request
    );
}

esp_err_t web_timetable_bridge_add(
    const web_timetable_entry_t *entry
)
{
    if (entry == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_timetable_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_TIMETABLE_COMMAND_ADD;

    request.entry =
        *entry;

    return submit_request(
        &request
    );
}

esp_err_t web_timetable_bridge_delete(
    size_t index
)
{
    web_timetable_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_TIMETABLE_COMMAND_DELETE;

    request.index =
        index;

    return submit_request(
        &request
    );
}

esp_err_t web_timetable_bridge_restore_defaults(void)
{
    web_timetable_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_TIMETABLE_COMMAND_RESTORE_DEFAULTS;

    return submit_request(
        &request
    );
}

/* =========================================================
 * Main-task functions
 * ========================================================= */

bool web_timetable_bridge_receive(
    web_timetable_request_t *request
)
{
    if (!bridge_ready ||
        command_queue == NULL ||
        request == NULL)
    {
        return false;
    }

    return
        xQueueReceive(
            command_queue,
            request,
            0
        ) == pdTRUE;
}

void web_timetable_bridge_complete(
    const web_timetable_request_t *request,
    esp_err_t result
)
{
    if (request == NULL)
    {
        return;
    }

    if (request->result_location != NULL)
    {
        *request->result_location =
            result;
    }

    if (request->requesting_task != NULL)
    {
        xTaskNotifyGive(
            request->requesting_task
        );
    }
}
