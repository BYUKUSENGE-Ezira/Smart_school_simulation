#include "announcement_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

static const char *TAG =
    "ANNOUNCEMENT";

/* =========================================================
 * Runtime state
 * ========================================================= */

static StaticSemaphore_t
    announcement_mutex_storage;

static SemaphoreHandle_t
    announcement_mutex = NULL;

static bool manager_ready = false;

static bool live_requested = false;
static bool emergency_active = false;

static announcement_state_t current_state =
    ANNOUNCEMENT_STATE_IDLE;

static uint32_t transition_count = 0U;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static announcement_state_t calculate_state(void)
{
    if (emergency_active)
    {
        return ANNOUNCEMENT_STATE_EMERGENCY;
    }

    if (live_requested)
    {
        return ANNOUNCEMENT_STATE_LIVE;
    }

    return ANNOUNCEMENT_STATE_IDLE;
}

static void update_state_locked(void)
{
    announcement_state_t new_state =
        calculate_state();

    if (new_state == current_state)
    {
        return;
    }

    announcement_state_t previous_state =
        current_state;

    current_state =
        new_state;

    transition_count++;

    ESP_LOGI(
        TAG,
        "State changed: %s -> %s",
        announcement_state_name(previous_state),
        announcement_state_name(current_state)
    );
}

static bool lock_manager(void)
{
    if (
        !manager_ready ||
        announcement_mutex == NULL
    )
    {
        return false;
    }

    return
        xSemaphoreTake(
            announcement_mutex,
            portMAX_DELAY
        ) == pdTRUE;
}

static void unlock_manager(void)
{
    xSemaphoreGive(
        announcement_mutex
    );
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t announcement_manager_init(void)
{
    if (manager_ready)
    {
        return ESP_OK;
    }

    announcement_mutex =
        xSemaphoreCreateMutexStatic(
            &announcement_mutex_storage
        );

    if (announcement_mutex == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create announcement mutex"
        );

        return ESP_ERR_NO_MEM;
    }

    live_requested = false;
    emergency_active = false;

    current_state =
        ANNOUNCEMENT_STATE_IDLE;

    transition_count = 0U;

    manager_ready = true;

    ESP_LOGI(
        TAG,
        "Announcement manager initialized"
    );

    return ESP_OK;
}

esp_err_t announcement_start_live(void)
{
    if (!lock_manager())
    {
        return ESP_ERR_INVALID_STATE;
    }

    live_requested = true;

    update_state_locked();

    unlock_manager();

    return ESP_OK;
}

esp_err_t announcement_stop_live(void)
{
    if (!lock_manager())
    {
        return ESP_ERR_INVALID_STATE;
    }

    live_requested = false;

    update_state_locked();

    unlock_manager();

    return ESP_OK;
}

esp_err_t announcement_start_emergency(void)
{
    if (!lock_manager())
    {
        return ESP_ERR_INVALID_STATE;
    }

    emergency_active = true;

    update_state_locked();

    unlock_manager();

    return ESP_OK;
}

esp_err_t announcement_stop_emergency(void)
{
    if (!lock_manager())
    {
        return ESP_ERR_INVALID_STATE;
    }

    emergency_active = false;

    update_state_locked();

    unlock_manager();

    return ESP_OK;
}

esp_err_t announcement_get_status(
    announcement_status_t *status
)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_manager())
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(
        status,
        0,
        sizeof(*status)
    );

    status->state =
        current_state;

    status->live_requested =
        live_requested;

    status->emergency_active =
        emergency_active;

    status->bell_blocked =
        current_state !=
        ANNOUNCEMENT_STATE_IDLE;

    status->transition_count =
        transition_count;

    unlock_manager();

    return ESP_OK;
}

bool announcement_is_active(void)
{
    if (!lock_manager())
    {
        return false;
    }

    bool active =
        current_state !=
        ANNOUNCEMENT_STATE_IDLE;

    unlock_manager();

    return active;
}

bool announcement_blocks_bell(void)
{
    return announcement_is_active();
}

const char *announcement_state_name(
    announcement_state_t state
)
{
    switch (state)
    {
        case ANNOUNCEMENT_STATE_IDLE:
            return "IDLE";

        case ANNOUNCEMENT_STATE_LIVE:
            return "LIVE";

        case ANNOUNCEMENT_STATE_EMERGENCY:
            return "EMERGENCY";

        default:
            return "UNKNOWN";
    }
}
