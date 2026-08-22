#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"

/* =========================================================
 * Configuration
 * ========================================================= */

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG =
    "WIFI_MANAGER";

/* =========================================================
 * Runtime state
 * ========================================================= */

static bool wifi_initialized = false;

static volatile wifi_manager_state_t
    wifi_state =
        WIFI_MANAGER_NOT_INITIALIZED;

static volatile uint32_t
    reconnect_attempt_count = 0;

static volatile uint32_t
    current_ipv4_address = 0;

static StaticEventGroup_t
    wifi_event_group_storage;

static EventGroupHandle_t
    wifi_event_group = NULL;

static esp_netif_t *station_netif =
    NULL;

static esp_event_handler_instance_t
    wifi_event_handler_instance;

static esp_event_handler_instance_t
    ip_event_handler_instance;

/* =========================================================
 * Internal event handler
 * ========================================================= */

static void wifi_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_argument;

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    )
    {
        wifi_state =
            WIFI_MANAGER_CONNECTING;

        esp_err_t result =
            esp_wifi_connect();

        if (result != ESP_OK)
        {
            wifi_state =
                WIFI_MANAGER_ERROR;

            ESP_LOGE(
                TAG,
                "Initial connection request failed: %s",
                esp_err_to_name(result)
            );
        }

        return;
    }

    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    )
    {
        wifi_state =
            WIFI_MANAGER_DISCONNECTED;

        current_ipv4_address = 0;

        if (wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        reconnect_attempt_count++;

        int disconnect_reason = -1;

        if (event_data != NULL)
        {
            const wifi_event_sta_disconnected_t
                *disconnect_event =
                    event_data;

            disconnect_reason =
                (int)
                    disconnect_event->reason;
        }

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected, reason=%d, reconnecting",
            disconnect_reason
        );

        wifi_state =
            WIFI_MANAGER_CONNECTING;

        esp_err_t result =
            esp_wifi_connect();

        if (result != ESP_OK)
        {
            wifi_state =
                WIFI_MANAGER_ERROR;

            ESP_LOGE(
                TAG,
                "Reconnection request failed: %s",
                esp_err_to_name(result)
            );
        }

        return;
    }

    if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    )
    {
        if (event_data == NULL)
        {
            wifi_state =
                WIFI_MANAGER_ERROR;

            return;
        }

        const ip_event_got_ip_t
            *ip_event =
                event_data;

        current_ipv4_address =
            ip_event->ip_info.ip.addr;

        reconnect_attempt_count = 0;

        wifi_state =
            WIFI_MANAGER_CONNECTED;

        if (wifi_event_group != NULL)
        {
            xEventGroupSetBits(
                wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        ESP_LOGI(
            TAG,
            "Connected, IP address: " IPSTR,
            IP2STR(
                &ip_event->ip_info.ip
            )
        );
    }
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t wifi_manager_init(
    const char *ssid,
    const char *password
)
{
    if (wifi_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL ||
        password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_length =
        strlen(ssid);

    size_t password_length =
        strlen(password);

    if (ssid_length == 0U ||
        ssid_length >
            sizeof(
                ((wifi_config_t *)0)->
                    sta.ssid
            ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (password_length >
        sizeof(
            ((wifi_config_t *)0)->
                sta.password
        ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_event_group =
        xEventGroupCreateStatic(
            &wifi_event_group_storage
        );

    if (wifi_event_group == NULL)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return ESP_ERR_NO_MEM;
    }

    esp_err_t result =
        esp_netif_init();

    if (
        result != ESP_OK &&
        result != ESP_ERR_INVALID_STATE
    )
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    result =
        esp_event_loop_create_default();

    if (
        result != ESP_OK &&
        result != ESP_ERR_INVALID_STATE
    )
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    station_netif =
        esp_netif_create_default_wifi_sta();

    if (station_netif == NULL)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initialization =
        WIFI_INIT_CONFIG_DEFAULT();

    result =
        esp_wifi_init(
            &initialization
        );

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    result =
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL,
            &wifi_event_handler_instance
        );

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    result =
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL,
            &ip_event_handler_instance
        );

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    wifi_config_t configuration = {0};

    memcpy(
        configuration.sta.ssid,
        ssid,
        ssid_length
    );

    memcpy(
        configuration.sta.password,
        password,
        password_length
    );

    /*
     * Accept open networks such as Wokwi-GUEST and secured
     * networks. Authentication is negotiated automatically.
     */
    configuration.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    configuration.sta.pmf_cfg.capable =
        true;

    configuration.sta.pmf_cfg.required =
        false;

    result =
        esp_wifi_set_mode(
            WIFI_MODE_STA
        );

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    result =
        esp_wifi_set_config(
            WIFI_IF_STA,
            &configuration
        );

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        return result;
    }

    wifi_state =
        WIFI_MANAGER_CONNECTING;

    wifi_initialized = true;

    result =
        esp_wifi_start();

    if (result != ESP_OK)
    {
        wifi_state =
            WIFI_MANAGER_ERROR;

        ESP_LOGE(
            TAG,
            "Wi-Fi start failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi station started"
    );

    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return wifi_state;
}

bool wifi_manager_is_connected(void)
{
    if (wifi_event_group == NULL)
    {
        return false;
    }

    EventBits_t bits =
        xEventGroupGetBits(
            wifi_event_group
        );

    return
        (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_ip_string(
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL ||
        buffer_size < 8U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ip4_addr_t address = {
        .addr = current_ipv4_address
    };

    int written =
        snprintf(
            buffer,
            buffer_size,
            IPSTR,
            IP2STR(&address)
        );

    if (written < 0 ||
        (size_t)written >= buffer_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

uint32_t wifi_manager_reconnect_attempts(void)
{
    return reconnect_attempt_count;
}

const char *wifi_manager_state_name(
    wifi_manager_state_t state
)
{
    switch (state)
    {
        case WIFI_MANAGER_NOT_INITIALIZED:
            return "NOT INITIALIZED";

        case WIFI_MANAGER_CONNECTING:
            return "CONNECTING";

        case WIFI_MANAGER_CONNECTED:
            return "CONNECTED";

        case WIFI_MANAGER_DISCONNECTED:
            return "DISCONNECTED";

        case WIFI_MANAGER_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}
