#include "web_event_bridge.h"

#include <string.h>

#include "freertos/queue.h"

#include "esp_log.h"

#define WEB_EVENT_QUEUE_LENGTH 4U

static const char *TAG =
    "WEB_EVENT";

static StaticQueue_t
    event_queue_control;

static uint8_t event_queue_storage[
    WEB_EVENT_QUEUE_LENGTH *
    sizeof(web_event_request_t)
];

static QueueHandle_t event_queue =
    NULL;

static bool bridge_ready = false;

/* =========================================================
 * Internal request function
 * ========================================================= */

static esp_err_t submit_request(
    web_event_request_t *request
)
{
    if (
        !bridge_ready ||
        event_queue == NULL ||
        request == NULL
    )
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
     * Remove any previous notification before waiting for
     * this command.
     */
    ulTaskNotifyTake(
        pdTRUE,
        0
    );

    BaseType_t queued =
        xQueueSend(
            event_queue,
            request,
            pdMS_TO_TICKS(1000U)
        );

    if (queued != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * The main alarm task completes the request and wakes
     * this HTTP task.
     */
    ulTaskNotifyTake(
        pdTRUE,
        portMAX_DELAY
    );

    return operation_result;
}

/* =========================================================
 * Initialization
 * ========================================================= */

esp_err_t web_event_bridge_init(void)
{
    if (bridge_ready)
    {
        return ESP_OK;
    }

    event_queue =
        xQueueCreateStatic(
            WEB_EVENT_QUEUE_LENGTH,
            sizeof(web_event_request_t),
            event_queue_storage,
            &event_queue_control
        );

    if (event_queue == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create event command queue"
        );

        return ESP_ERR_NO_MEM;
    }

    bridge_ready = true;

    ESP_LOGI(
        TAG,
        "Event command bridge initialized"
    );

    return ESP_OK;
}

/* =========================================================
 * HTTP-task functions
 * ========================================================= */

esp_err_t web_event_bridge_get_all(
    event_log_record_t *records,
    size_t capacity,
    size_t *count
)
{
    if (
        records == NULL ||
        count == NULL ||
        capacity == 0U
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0U;

    web_event_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_EVENT_COMMAND_GET_ALL;

    request.output_records =
        records;

    request.output_capacity =
        capacity;

    request.output_count =
        count;

    return submit_request(
        &request
    );
}

esp_err_t web_event_bridge_clear(void)
{
    web_event_request_t request;

    memset(
        &request,
        0,
        sizeof(request)
    );

    request.type =
        WEB_EVENT_COMMAND_CLEAR;

    return submit_request(
        &request
    );
}

/* =========================================================
 * Main-task functions
 * ========================================================= */

bool web_event_bridge_receive(
    web_event_request_t *request
)
{
    if (
        !bridge_ready ||
        event_queue == NULL ||
        request == NULL
    )
    {
        return false;
    }

    return
        xQueueReceive(
            event_queue,
            request,
            0
        ) == pdTRUE;
}

void web_event_bridge_complete(
    const web_event_request_t *request,
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
