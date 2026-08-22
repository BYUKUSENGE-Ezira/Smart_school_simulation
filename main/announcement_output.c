#include "announcement_output.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "announcement_manager.h"

static const char *TAG =
    "ANNOUNCEMENT_OUTPUT";

static bool output_ready = false;
static bool output_active = false;

esp_err_t announcement_output_init(void)
{
    if (output_ready)
    {
        return ESP_OK;
    }

    gpio_config_t configuration = {
        .pin_bit_mask =
            1ULL << ANNOUNCEMENT_OUTPUT_GPIO,

        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t result =
        gpio_config(&configuration);

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Output GPIO configuration failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    result =
        gpio_set_level(
            ANNOUNCEMENT_OUTPUT_GPIO,
            0
        );

    if (result != ESP_OK)
    {
        return result;
    }

    output_active = false;
    output_ready = true;

    ESP_LOGI(
        TAG,
        "PA output ready on GPIO%d",
        ANNOUNCEMENT_OUTPUT_GPIO
    );

    return ESP_OK;
}

esp_err_t announcement_output_sync(void)
{
    if (!output_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    announcement_status_t status;

    esp_err_t result =
        announcement_get_status(
            &status
        );

    if (result != ESP_OK)
    {
        return result;
    }

    bool requested_active =
        status.state !=
        ANNOUNCEMENT_STATE_IDLE;

    if (requested_active == output_active)
    {
        return ESP_OK;
    }

    result =
        gpio_set_level(
            ANNOUNCEMENT_OUTPUT_GPIO,
            requested_active ? 1 : 0
        );

    if (result != ESP_OK)
    {
        return result;
    }

    output_active =
        requested_active;

    ESP_LOGI(
        TAG,
        "PA output %s for state %s",
        output_active ? "ON" : "OFF",
        announcement_state_name(
            status.state
        )
    );

    return ESP_OK;
}

bool announcement_output_is_active(void)
{
    return output_ready &&
           output_active;
}
