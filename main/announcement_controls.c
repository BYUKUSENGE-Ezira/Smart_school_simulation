#include "announcement_controls.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "announcement_manager.h"

#define BUTTON_DEBOUNCE_MS 50

typedef struct
{
    bool raw_pressed;
    bool stable_pressed;
    int64_t last_change_ms;
} announcement_button_t;

static const char *TAG =
    "ANNOUNCEMENT_INPUT";

static bool controls_ready = false;

static bool emergency_latched = false;

static announcement_button_t ptt_button = {0};
static announcement_button_t emergency_button = {0};

static bool gpio_is_pressed(
    gpio_num_t gpio_number
)
{
    /*
     * Buttons use the ESP32 internal pull-up.
     * Pressing the button connects the GPIO to GND.
     */
    return gpio_get_level(gpio_number) == 0;
}

static bool update_button(
    announcement_button_t *button,
    gpio_num_t gpio_number,
    bool *pressed_event,
    bool *released_event
)
{
    if (
        button == NULL ||
        pressed_event == NULL ||
        released_event == NULL
    )
    {
        return false;
    }

    *pressed_event = false;
    *released_event = false;

    bool current_raw =
        gpio_is_pressed(gpio_number);

    int64_t current_time_ms =
        esp_timer_get_time() / 1000;

    if (current_raw != button->raw_pressed)
    {
        button->raw_pressed =
            current_raw;

        button->last_change_ms =
            current_time_ms;
    }

    if (
        current_raw != button->stable_pressed &&
        current_time_ms - button->last_change_ms >=
            BUTTON_DEBOUNCE_MS
    )
    {
        button->stable_pressed =
            current_raw;

        if (current_raw)
        {
            *pressed_event = true;
        }
        else
        {
            *released_event = true;
        }

        return true;
    }

    return false;
}

esp_err_t announcement_controls_init(void)
{
    if (controls_ready)
    {
        return ESP_OK;
    }

    gpio_config_t configuration = {
        .pin_bit_mask =
            (1ULL << ANNOUNCEMENT_PTT_GPIO) |
            (1ULL << ANNOUNCEMENT_EMERGENCY_GPIO),

        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t result =
        gpio_config(&configuration);

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    int64_t current_time_ms =
        esp_timer_get_time() / 1000;

    ptt_button.raw_pressed =
        gpio_is_pressed(
            ANNOUNCEMENT_PTT_GPIO
        );

    ptt_button.stable_pressed =
        ptt_button.raw_pressed;

    ptt_button.last_change_ms =
        current_time_ms;

    emergency_button.raw_pressed =
        gpio_is_pressed(
            ANNOUNCEMENT_EMERGENCY_GPIO
        );

    emergency_button.stable_pressed =
        emergency_button.raw_pressed;

    emergency_button.last_change_ms =
        current_time_ms;

    emergency_latched = false;
    controls_ready = true;

    ESP_LOGI(
        TAG,
        "PTT button ready on GPIO%d",
        ANNOUNCEMENT_PTT_GPIO
    );

    ESP_LOGI(
        TAG,
        "Emergency button ready on GPIO%d",
        ANNOUNCEMENT_EMERGENCY_GPIO
    );

    return ESP_OK;
}

esp_err_t announcement_controls_poll(void)
{
    if (!controls_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool pressed_event = false;
    bool released_event = false;

    update_button(
        &ptt_button,
        ANNOUNCEMENT_PTT_GPIO,
        &pressed_event,
        &released_event
    );

    if (pressed_event)
    {
        ESP_LOGW(
            TAG,
            "Director push-to-talk pressed"
        );

        announcement_start_live();
    }

    if (released_event)
    {
        ESP_LOGI(
            TAG,
            "Director push-to-talk released"
        );

        announcement_stop_live();
    }

    pressed_event = false;
    released_event = false;

    update_button(
        &emergency_button,
        ANNOUNCEMENT_EMERGENCY_GPIO,
        &pressed_event,
        &released_event
    );

    /*
     * The emergency button is latched:
     *
     * First press  -> activate emergency
     * Second press -> cancel emergency
     */
    if (pressed_event)
    {
        emergency_latched =
            !emergency_latched;

        if (emergency_latched)
        {
            ESP_LOGE(
                TAG,
                "Emergency announcement activated"
            );

            announcement_start_emergency();
        }
        else
        {
            ESP_LOGW(
                TAG,
                "Emergency announcement cancelled"
            );

            announcement_stop_emergency();
        }
    }

    return ESP_OK;
}
