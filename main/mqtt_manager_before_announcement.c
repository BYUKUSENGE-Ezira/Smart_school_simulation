#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

/*
 * Public broker used only for simulation.
 *
 * Real deployment should use a private authenticated broker
 * with TLS.
 */
#define MQTT_BROKER_URI \
    "ws://34.243.217.54:8083/mqtt"

#define MQTT_TOPIC_PREFIX_ROOT \
    "darho/smart-alarm/simulation"

#define MQTT_CLIENT_ID_SIZE 48
#define MQTT_TOPIC_SIZE 128
#define MQTT_PAYLOAD_SIZE 640

static const char *TAG =
    "MQTT_MANAGER";

static esp_mqtt_client_handle_t
    mqtt_client = NULL;

static volatile bool
    mqtt_connected = false;

static char mqtt_client_id[
    MQTT_CLIENT_ID_SIZE
];

static char mqtt_topic_prefix[
    MQTT_TOPIC_SIZE
];

static char mqtt_status_topic[
    MQTT_TOPIC_SIZE
];

static char mqtt_availability_topic[
    MQTT_TOPIC_SIZE
];

/*
 * Static publishing buffer prevents a large JSON buffer
 * from consuming the main task stack.
 */
static char mqtt_publish_payload[
    MQTT_PAYLOAD_SIZE
];

static portMUX_TYPE mqtt_diagnostic_lock =
    portMUX_INITIALIZER_UNLOCKED;

static mqtt_diagnostics_t mqtt_diagnostics = {
    .connected = false,
    .disconnect_count = 0U,
    .summary = "MQTT client not started"
};

static void update_diagnostic_summary(
    const char *summary
)
{
    if (summary == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    snprintf(
        mqtt_diagnostics.summary,
        sizeof(mqtt_diagnostics.summary),
        "%s",
        summary
    );

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );
}

static void mark_mqtt_connected(void)
{
    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    mqtt_diagnostics.connected = true;

    mqtt_diagnostics.error_type = 0;
    mqtt_diagnostics.socket_errno = 0;
    mqtt_diagnostics.tls_error = 0;
    mqtt_diagnostics.tls_stack_error = 0;
    mqtt_diagnostics.connect_return_code = 0;

    snprintf(
        mqtt_diagnostics.summary,
        sizeof(mqtt_diagnostics.summary),
        "Connected successfully"
    );

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );
}

static void mark_mqtt_disconnected(void)
{
    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    mqtt_diagnostics.connected = false;
    mqtt_diagnostics.disconnect_count++;

    if (
        mqtt_diagnostics.error_type == 0 &&
        mqtt_diagnostics.socket_errno == 0 &&
        mqtt_diagnostics.tls_error == 0
    )
    {
        snprintf(
            mqtt_diagnostics.summary,
            sizeof(mqtt_diagnostics.summary),
            "Broker disconnected without detailed error"
        );
    }

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );
}

static void capture_mqtt_error(
    const esp_mqtt_error_codes_t *error
)
{
    mqtt_diagnostics_t updated;

    memset(
        &updated,
        0,
        sizeof(updated)
    );

    updated.connected = false;

    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    updated.disconnect_count =
        mqtt_diagnostics.disconnect_count;

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );

    if (error != NULL)
    {
        updated.error_type =
            (int32_t)error->error_type;

        updated.socket_errno =
            (int32_t)
                error->esp_transport_sock_errno;

        updated.tls_error =
            (int32_t)
                error->esp_tls_last_esp_err;

        updated.tls_stack_error =
            (int32_t)
                error->esp_tls_stack_err;

        updated.connect_return_code =
            (int32_t)
                error->connect_return_code;
    }

    const char *socket_message =
        updated.socket_errno != 0
            ? strerror(
                updated.socket_errno
            )
            : "none";

    snprintf(
        updated.summary,
        sizeof(updated.summary),

        "type=%ld socket=%ld (%s) "
        "tls=0x%lx stack=%ld return=%ld",

        (long)updated.error_type,
        (long)updated.socket_errno,
        socket_message,
        (unsigned long)updated.tls_error,
        (long)updated.tls_stack_error,
        (long)updated.connect_return_code
    );

    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    mqtt_diagnostics =
        updated;

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );
}


/* =========================================================
 * Device identity and topics
 * ========================================================= */

static esp_err_t build_device_identity(void)
{
    uint8_t mac[6];

    esp_err_t result =
        esp_read_mac(
            mac,
            ESP_MAC_WIFI_STA
        );

    if (result != ESP_OK)
    {
        return result;
    }

    int written =
        snprintf(
            mqtt_client_id,
            sizeof(mqtt_client_id),

            "smart-alarm-%02x%02x%02x%02x%02x%02x",

            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(mqtt_client_id)
    )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    written =
        snprintf(
            mqtt_topic_prefix,
            sizeof(mqtt_topic_prefix),

            MQTT_TOPIC_PREFIX_ROOT
            "/%02x%02x%02x%02x%02x%02x",

            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(mqtt_topic_prefix)
    )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    written =
        snprintf(
            mqtt_status_topic,
            sizeof(mqtt_status_topic),
            "%s/status",
            mqtt_topic_prefix
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(mqtt_status_topic)
    )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    written =
        snprintf(
            mqtt_availability_topic,
            sizeof(mqtt_availability_topic),
            "%s/availability",
            mqtt_topic_prefix
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(mqtt_availability_topic)
    )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* =========================================================
 * MQTT events
 * ========================================================= */

static void mqtt_event_handler(
    void *handler_arguments,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_arguments;
    (void)event_base;

    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
        {
            mqtt_connected = true;

            mark_mqtt_connected();

            ESP_LOGI(
                TAG,
                "Connected to MQTT broker"
            );

            ESP_LOGI(
                TAG,
                "Topic prefix: %s",
                mqtt_topic_prefix
            );

            /*
             * Retained online state.
             */
            esp_mqtt_client_publish(
                mqtt_client,
                mqtt_availability_topic,
                "online",
                0,
                1,
                true
            );

            break;
        }

        case MQTT_EVENT_DISCONNECTED:
        {
            mqtt_connected = false;

            mark_mqtt_disconnected();

            ESP_LOGW(
                TAG,
                "Disconnected from MQTT broker"
            );

            break;
        }

        case MQTT_EVENT_ERROR:
        {
            mqtt_connected = false;

            capture_mqtt_error(
                event != NULL
                    ? event->error_handle
                    : NULL
            );

            ESP_LOGE(
                TAG,
                "MQTT transport error"
            );

            break;
        }

        default:
        {
            break;
        }
    }
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t mqtt_manager_start(void)
{
    if (mqtt_client != NULL)
    {
        return ESP_OK;
    }

    esp_err_t result =
        build_device_identity();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Unable to build MQTT identity: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    esp_mqtt_client_config_t configuration = {
        .broker.address.uri =
            MQTT_BROKER_URI,

        .credentials.client_id =
            mqtt_client_id,

        .session.last_will.topic =
            mqtt_availability_topic,

        .session.last_will.msg =
            "offline",

        .session.last_will.msg_len =
            0,

        .session.last_will.qos =
            1,

        .session.last_will.retain =
            true,

        .network.reconnect_timeout_ms =
            5000
    };

    mqtt_client =
        esp_mqtt_client_init(
            &configuration
        );

    if (mqtt_client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    result =
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        );

    if (result != ESP_OK)
    {
        esp_mqtt_client_destroy(
            mqtt_client
        );

        mqtt_client = NULL;

        return result;
    }

    result =
        esp_mqtt_client_start(
            mqtt_client
        );

    if (result != ESP_OK)
    {
        esp_mqtt_client_destroy(
            mqtt_client
        );

        mqtt_client = NULL;

        return result;
    }

    update_diagnostic_summary(
        "MQTT client started; waiting for broker"
    );

    ESP_LOGI(
        TAG,
        "MQTT client started"
    );

    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (mqtt_client == NULL)
    {
        return ESP_OK;
    }

    mqtt_connected = false;

    esp_mqtt_client_publish(
        mqtt_client,
        mqtt_availability_topic,
        "offline",
        0,
        1,
        true
    );

    esp_err_t stop_result =
        esp_mqtt_client_stop(
            mqtt_client
        );

    esp_err_t destroy_result =
        esp_mqtt_client_destroy(
            mqtt_client
        );

    mqtt_client = NULL;

    if (stop_result != ESP_OK)
    {
        return stop_result;
    }

    return destroy_result;
}

bool mqtt_manager_is_connected(void)
{
    return mqtt_connected;
}

esp_err_t mqtt_manager_publish_status(
    const mqtt_alarm_status_t *status
)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        mqtt_client == NULL ||
        !mqtt_connected
    )
    {
        return ESP_ERR_INVALID_STATE;
    }

    int written =
        snprintf(
            mqtt_publish_payload,
            sizeof(mqtt_publish_payload),

            "{"
            "\"device_id\":\"%s\","
            "\"current_time\":\"%s\","
            "\"next_bell\":\"%s\","
            "\"auto_enabled\":%s,"
            "\"alarm_active\":%s,"
            "\"rtc_ready\":%s,"
            "\"timetable_count\":%lu,"
            "\"event_log_count\":%lu,"
            "\"ring_duration_seconds\":%lu,"
            "\"uptime_seconds\":%lu"
            "}",

            mqtt_client_id,
            status->current_time,
            status->next_bell,

            status->auto_enabled
                ? "true"
                : "false",

            status->alarm_active
                ? "true"
                : "false",

            status->rtc_ready
                ? "true"
                : "false",

            (unsigned long)
                status->timetable_count,

            (unsigned long)
                status->event_log_count,

            (unsigned long)
                status->ring_duration_seconds,

            (unsigned long)
                status->uptime_seconds
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(mqtt_publish_payload)
    )
    {
        return ESP_ERR_INVALID_SIZE;
    }

    int message_id =
        esp_mqtt_client_enqueue(
            mqtt_client,
            mqtt_status_topic,
            mqtt_publish_payload,
            written,
            1,
            true,
            true
        );

    if (message_id < 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

const char *mqtt_manager_topic_prefix(void)
{
    return mqtt_topic_prefix;
}


const char *mqtt_manager_broker_uri(void)
{
    return MQTT_BROKER_URI;
}

void mqtt_manager_get_diagnostics(
    mqtt_diagnostics_t *diagnostics
)
{
    if (diagnostics == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(
        &mqtt_diagnostic_lock
    );

    *diagnostics =
        mqtt_diagnostics;

    taskEXIT_CRITICAL(
        &mqtt_diagnostic_lock
    );
}



