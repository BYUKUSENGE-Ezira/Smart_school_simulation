#include "event_log.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

/* =========================================================
 * NVS configuration
 * ========================================================= */

#define EVENT_LOG_NAMESPACE "school_log"
#define EVENT_LOG_STATE_KEY "event_state"

#define EVENT_LOG_STORAGE_VERSION 1U

static const char *TAG = "EVENT_LOG";

/* =========================================================
 * Internal storage
 * ========================================================= */

typedef struct
{
    uint32_t version;
    uint32_t next_sequence;

    uint16_t count;
    uint16_t next_index;

    event_log_record_t records[
        EVENT_LOG_MAX_RECORDS
    ];
} event_log_storage_t;

static event_log_storage_t log_storage;

static bool log_initialized = false;

/* =========================================================
 * Internal functions
 * ========================================================= */

static void reset_log_storage(void)
{
    memset(
        &log_storage,
        0,
        sizeof(log_storage)
    );

    log_storage.version =
        EVENT_LOG_STORAGE_VERSION;

    log_storage.next_sequence = 1;

    log_storage.count = 0;
    log_storage.next_index = 0;
}

static bool storage_is_valid(void)
{
    if (log_storage.version !=
        EVENT_LOG_STORAGE_VERSION)
    {
        return false;
    }

    if (log_storage.count >
        EVENT_LOG_MAX_RECORDS)
    {
        return false;
    }

    if (log_storage.next_index >=
        EVENT_LOG_MAX_RECORDS)
    {
        return false;
    }

    if (log_storage.next_sequence == 0)
    {
        return false;
    }

    return true;
}

static esp_err_t save_storage(void)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            EVENT_LOG_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    result =
        nvs_set_blob(
            handle,
            EVENT_LOG_STATE_KEY,
            &log_storage,
            sizeof(log_storage)
        );

    if (result == ESP_OK)
    {
        result =
            nvs_commit(handle);
    }

    nvs_close(handle);

    return result;
}

static esp_err_t load_storage(void)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            EVENT_LOG_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    size_t stored_size = 0;

    result =
        nvs_get_blob(
            handle,
            EVENT_LOG_STATE_KEY,
            NULL,
            &stored_size
        );

    if (result != ESP_OK)
    {
        nvs_close(handle);
        return result;
    }

    if (stored_size !=
        sizeof(log_storage))
    {
        nvs_close(handle);

        return ESP_ERR_INVALID_SIZE;
    }

    result =
        nvs_get_blob(
            handle,
            EVENT_LOG_STATE_KEY,
            &log_storage,
            &stored_size
        );

    nvs_close(handle);

    if (result != ESP_OK)
    {
        return result;
    }

    if (!storage_is_valid())
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t event_log_init(void)
{
    esp_err_t result =
        load_storage();

    if (result == ESP_OK)
    {
        log_initialized = true;

        ESP_LOGI(
            TAG,
            "Loaded %u event records",
            (unsigned int)
                log_storage.count
        );

        return ESP_OK;
    }

    /*
     * Create a new empty event log when no valid
     * saved log exists.
     */
    ESP_LOGW(
        TAG,
        "Creating new event log: %s",
        esp_err_to_name(result)
    );

    reset_log_storage();

    result =
        save_storage();

    if (result == ESP_OK)
    {
        log_initialized = true;

        ESP_LOGI(
            TAG,
            "New event log created"
        );
    }
    else
    {
        log_initialized = false;

        ESP_LOGE(
            TAG,
            "Failed to create event log: %s",
            esp_err_to_name(result)
        );
    }

    return result;
}

esp_err_t event_log_add(
    event_log_type_t type,
    const event_log_time_t *time,
    int32_t value
)
{
    if (!log_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (type < EVENT_LOG_SYSTEM_START ||
        type > EVENT_LOG_LOGS_CLEARED)
    {
        return ESP_ERR_INVALID_ARG;
    }

    event_log_record_t record;

    memset(
        &record,
        0,
        sizeof(record)
    );

    record.sequence =
        log_storage.next_sequence;

    record.type =
        type;

    record.value =
        value;

    if (time != NULL)
    {
        record.time =
            *time;
    }

    /*
     * Save the record at the current circular-buffer
     * write position.
     */
    log_storage.records[
        log_storage.next_index
    ] = record;

    log_storage.next_index++;

    if (log_storage.next_index >=
        EVENT_LOG_MAX_RECORDS)
    {
        log_storage.next_index = 0;
    }

    if (log_storage.count <
        EVENT_LOG_MAX_RECORDS)
    {
        log_storage.count++;
    }

    log_storage.next_sequence++;

    /*
     * Sequence number zero is reserved.
     */
    if (log_storage.next_sequence == 0)
    {
        log_storage.next_sequence = 1;
    }

    esp_err_t result =
        save_storage();

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Event saved: #%lu %s",
            (unsigned long)
                record.sequence,
            event_log_type_name(type)
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to save event: %s",
            esp_err_to_name(result)
        );
    }

    return result;
}

size_t event_log_count(void)
{
    if (!log_initialized)
    {
        return 0;
    }

    return log_storage.count;
}

bool event_log_get_newest(
    size_t newest_offset,
    event_log_record_t *record
)
{
    if (!log_initialized ||
        record == NULL)
    {
        return false;
    }

    if (newest_offset >=
        log_storage.count)
    {
        return false;
    }

    /*
     * next_index points to the location where the next
     * record will be written.
     *
     * Therefore next_index - 1 is the newest record.
     */
    int index =
        (int)log_storage.next_index -
        1 -
        (int)newest_offset;

    while (index < 0)
    {
        index +=
            EVENT_LOG_MAX_RECORDS;
    }

    *record =
        log_storage.records[index];

    return true;
}

esp_err_t event_log_clear(void)
{
    if (!log_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    reset_log_storage();

    esp_err_t result =
        save_storage();

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Event log cleared"
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to clear event log: %s",
            esp_err_to_name(result)
        );
    }

    return result;
}

const char *event_log_type_name(
    event_log_type_t type
)
{
    switch (type)
    {
        case EVENT_LOG_SYSTEM_START:
            return "SYSTEM START";

        case EVENT_LOG_SCHEDULED_RING:
            return "SCHEDULED RING";

        case EVENT_LOG_MANUAL_RING:
            return "MANUAL RING";

        case EVENT_LOG_BELL_TEST:
            return "BELL TEST";

        case EVENT_LOG_ALARM_STOPPED:
            return "ALARM STOPPED";

        case EVENT_LOG_AUTO_ENABLED:
            return "AUTO ENABLED";

        case EVENT_LOG_AUTO_DISABLED:
            return "AUTO DISABLED";

        case EVENT_LOG_RTC_ERROR:
            return "RTC ERROR";

        case EVENT_LOG_RTC_RECOVERED:
            return "RTC RECOVERED";

        case EVENT_LOG_TIME_CHANGED:
            return "TIME CHANGED";

        case EVENT_LOG_DATE_CHANGED:
            return "DATE CHANGED";

        case EVENT_LOG_BELL_ADDED:
            return "BELL ADDED";

        case EVENT_LOG_BELL_DELETED:
            return "BELL DELETED";

        case EVENT_LOG_DEFAULTS_RESTORED:
            return "DEFAULT RESTORED";

        case EVENT_LOG_DURATION_CHANGED:
            return "DURATION CHANGED";

        case EVENT_LOG_LOGS_CLEARED:
            return "LOGS CLEARED";

        default:
            return "UNKNOWN EVENT";
    }
}
