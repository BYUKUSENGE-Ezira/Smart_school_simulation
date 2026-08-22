#include "system_watchdog.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG =
    "SYSTEM_WATCHDOG";

static bool watchdog_ready = false;

esp_err_t system_watchdog_init(
    uint32_t timeout_ms
)
{
    if (timeout_ms < 1000U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_task_wdt_config_t configuration = {
        .timeout_ms = timeout_ms,

        /*
         * We subscribe the required task manually.
         * Idle tasks are not monitored here.
         */
        .idle_core_mask = 0,

        /*
         * Produce a panic and restart when the monitored
         * task fails to feed the watchdog.
         */
        .trigger_panic = true
    };

    esp_err_t result =
        esp_task_wdt_init(
            &configuration
        );

    /*
     * ESP-IDF may already have initialized the Task
     * Watchdog through sdkconfig. Reconfigure it instead
     * of treating that situation as a failure.
     */
    if (result ==
        ESP_ERR_INVALID_STATE)
    {
        result =
            esp_task_wdt_reconfigure(
                &configuration
            );
    }

    if (result != ESP_OK)
    {
        watchdog_ready = false;

        ESP_LOGE(
            TAG,
            "Watchdog initialization failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    /*
     * NULL means the task currently executing this code.
     */
    result =
        esp_task_wdt_add(NULL);

    if (result != ESP_OK)
    {
        /*
         * The task may already be subscribed.
         */
        esp_err_t status =
            esp_task_wdt_status(NULL);

        if (status != ESP_OK)
        {
            watchdog_ready = false;

            ESP_LOGE(
                TAG,
                "Failed to subscribe task: %s",
                esp_err_to_name(result)
            );

            return result;
        }
    }

    watchdog_ready = true;

    ESP_LOGI(
        TAG,
        "Task watchdog enabled with %lu ms timeout",
        (unsigned long)timeout_ms
    );

    return ESP_OK;
}

esp_err_t system_watchdog_feed(void)
{
    if (!watchdog_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_task_wdt_reset();
}

bool system_watchdog_is_ready(void)
{
    return watchdog_ready;
}
