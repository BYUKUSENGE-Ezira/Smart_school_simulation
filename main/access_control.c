#include "access_control.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

/* =========================================================
 * Configuration
 * ========================================================= */

#define ACCESS_NVS_NAMESPACE "alarm_access"
#define ACCESS_PIN_HASH_KEY "pin_hash"

#define DEFAULT_ADMIN_PIN "1234"

/*
 * This salt makes the stored value different from a
 * direct hash of the entered PIN.
 *
 * This protects against casual reading of the PIN from NVS,
 * but it is not a replacement for NVS encryption.
 */
#define PIN_HASH_SALT 0x7A31C5D9U

static const char *TAG = "ACCESS_CONTROL";

/* =========================================================
 * Runtime state
 * ========================================================= */

static bool access_initialized = false;

static uint32_t saved_pin_hash = 0;

static uint8_t failed_attempts = 0;

static bool lockout_active = false;

static uint32_t lockout_end_ms = 0;

/* =========================================================
 * Internal functions
 * ========================================================= */

static uint32_t calculate_pin_hash(
    const char *pin
)
{
    /*
     * FNV-1a based derived value.
     *
     * The actual PIN is never stored directly.
     */
    uint32_t hash =
        2166136261U ^
        PIN_HASH_SALT;

    size_t length =
        strlen(pin);

    for (size_t i = 0;
         i < length;
         i++)
    {
        hash ^=
            (uint8_t)pin[i];

        hash *=
            16777619U;
    }

    hash ^=
        (uint32_t)length;

    hash *=
        16777619U;

    return hash;
}

static esp_err_t save_pin_hash(
    uint32_t pin_hash
)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            ACCESS_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    result =
        nvs_set_u32(
            handle,
            ACCESS_PIN_HASH_KEY,
            pin_hash
        );

    if (result == ESP_OK)
    {
        result =
            nvs_commit(handle);
    }

    nvs_close(handle);

    return result;
}

static esp_err_t load_or_create_pin_hash(void)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            ACCESS_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    uint32_t stored_hash = 0;

    result =
        nvs_get_u32(
            handle,
            ACCESS_PIN_HASH_KEY,
            &stored_hash
        );

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        stored_hash =
            calculate_pin_hash(
                DEFAULT_ADMIN_PIN
            );

        result =
            nvs_set_u32(
                handle,
                ACCESS_PIN_HASH_KEY,
                stored_hash
            );

        if (result == ESP_OK)
        {
            result =
                nvs_commit(handle);
        }

        if (result == ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Default administrator PIN created"
            );
        }
    }

    if (result == ESP_OK)
    {
        saved_pin_hash =
            stored_hash;
    }

    nvs_close(handle);

    return result;
}

static bool lockout_is_active(
    uint32_t now_ms,
    uint32_t *remaining_ms
)
{
    if (!lockout_active)
    {
        if (remaining_ms != NULL)
        {
            *remaining_ms = 0;
        }

        return false;
    }

    /*
     * Signed subtraction handles the uint32_t timer
     * wrapping around.
     */
    int32_t time_difference =
        (int32_t)(
            now_ms -
            lockout_end_ms
        );

    if (time_difference >= 0)
    {
        lockout_active = false;
        lockout_end_ms = 0;
        failed_attempts = 0;

        if (remaining_ms != NULL)
        {
            *remaining_ms = 0;
        }

        ESP_LOGI(
            TAG,
            "Administrator PIN lockout ended"
        );

        return false;
    }

    if (remaining_ms != NULL)
    {
        *remaining_ms =
            lockout_end_ms -
            now_ms;
    }

    return true;
}

/* =========================================================
 * Public functions
 * ========================================================= */

bool access_control_pin_is_valid(
    const char *pin
)
{
    if (pin == NULL)
    {
        return false;
    }

    size_t length =
        strlen(pin);

    if (length <
            ACCESS_PIN_MIN_LENGTH ||
        length >
            ACCESS_PIN_MAX_LENGTH)
    {
        return false;
    }

    for (size_t i = 0;
         i < length;
         i++)
    {
        if (pin[i] < '0' ||
            pin[i] > '9')
        {
            return false;
        }
    }

    return true;
}

esp_err_t access_control_init(void)
{
    failed_attempts = 0;

    lockout_active = false;
    lockout_end_ms = 0;

    esp_err_t result =
        load_or_create_pin_hash();

    if (result == ESP_OK)
    {
        access_initialized = true;

        ESP_LOGI(
            TAG,
            "Administrator access control initialized"
        );
    }
    else
    {
        access_initialized = false;

        ESP_LOGE(
            TAG,
            "Access control initialization failed: %s",
            esp_err_to_name(result)
        );
    }

    return result;
}

access_result_t access_control_verify(
    const char *pin,
    uint32_t now_ms,
    uint32_t *remaining_lockout_ms
)
{
    if (remaining_lockout_ms != NULL)
    {
        *remaining_lockout_ms = 0;
    }

    if (!access_initialized)
    {
        return ACCESS_RESULT_ERROR;
    }

    if (lockout_is_active(
            now_ms,
            remaining_lockout_ms
        ))
    {
        return ACCESS_RESULT_LOCKED;
    }

    if (!access_control_pin_is_valid(pin))
    {
        failed_attempts++;

        ESP_LOGW(
            TAG,
            "Invalid administrator PIN format"
        );
    }
    else
    {
        uint32_t entered_hash =
            calculate_pin_hash(pin);

        if (entered_hash ==
            saved_pin_hash)
        {
            failed_attempts = 0;

            ESP_LOGI(
                TAG,
                "Administrator access granted"
            );

            return ACCESS_RESULT_GRANTED;
        }

        failed_attempts++;

        ESP_LOGW(
            TAG,
            "Incorrect administrator PIN"
        );
    }

    if (failed_attempts >=
        ACCESS_MAX_FAILED_ATTEMPTS)
    {
        failed_attempts = 0;

        lockout_active = true;

        lockout_end_ms =
            now_ms +
            ACCESS_LOCKOUT_MS;

        if (remaining_lockout_ms != NULL)
        {
            *remaining_lockout_ms =
                ACCESS_LOCKOUT_MS;
        }

        ESP_LOGW(
            TAG,
            "Administrator access locked for %lu ms",
            (unsigned long)
                ACCESS_LOCKOUT_MS
        );

        return ACCESS_RESULT_LOCKED;
    }

    return ACCESS_RESULT_DENIED;
}

esp_err_t access_control_change_pin(
    const char *new_pin
)
{
    if (!access_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!access_control_pin_is_valid(
            new_pin
        ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t new_hash =
        calculate_pin_hash(new_pin);

    esp_err_t result =
        save_pin_hash(new_hash);

    if (result == ESP_OK)
    {
        saved_pin_hash =
            new_hash;

        failed_attempts = 0;

        lockout_active = false;
        lockout_end_ms = 0;

        ESP_LOGI(
            TAG,
            "Administrator PIN changed"
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to save administrator PIN: %s",
            esp_err_to_name(result)
        );
    }

    return result;
}

uint8_t access_control_failed_attempts(void)
{
    return failed_attempts;
}
