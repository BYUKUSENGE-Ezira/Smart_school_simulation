#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "event_log.h"
#include "schedule_guard.h"
#include "access_control.h"
#include "system_watchdog.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "mqtt_manager.h"
#include "announcement_manager.h"
#include "announcement_controls.h"
#include "announcement_output.h"
#include "web_timetable_bridge.h"
#include "web_event_bridge.h"
#include "web_auth.h"

/* =========================================================
 * Pin configuration
 * ========================================================= */

#define LED_PIN GPIO_NUM_2
#define BUZZER_PIN GPIO_NUM_18
#define MANUAL_BUTTON_PIN GPIO_NUM_19
#define RELAY_PIN GPIO_NUM_23
#define AUTO_BUTTON_PIN GPIO_NUM_25

#define I2C_SDA_PIN GPIO_NUM_21
#define I2C_SCL_PIN GPIO_NUM_22

#define KEYPAD_R1_PIN GPIO_NUM_13
#define KEYPAD_R2_PIN GPIO_NUM_14
#define KEYPAD_R3_PIN GPIO_NUM_26
#define KEYPAD_R4_PIN GPIO_NUM_27

#define KEYPAD_C1_PIN GPIO_NUM_32
#define KEYPAD_C2_PIN GPIO_NUM_33
#define KEYPAD_C3_PIN GPIO_NUM_16
#define KEYPAD_C4_PIN GPIO_NUM_17

/* =========================================================
 * Device addresses
 * ========================================================= */

#define RTC_ADDRESS 0x68
#define LCD_ADDRESS 0x27

/* =========================================================
 * General configuration
 * ========================================================= */

#define DEFAULT_RING_DURATION_MS 3000U

#define MIN_RING_DURATION_SECONDS 1U
#define MAX_RING_DURATION_SECONDS 60U

#define BUTTON_DEBOUNCE_MS 50
#define KEYPAD_DEBOUNCE_MS 60

#define CONFIG_TIMEOUT_MS 30000
#define CONFIG_MESSAGE_MS 1600

#define RTC_STABLE_READINGS_REQUIRED 3

/*
 * A scheduled bell may still ring when the controller
 * notices it within this number of seconds.
 *
 * Larger jumps are treated as power/time faults and old
 * bells are not replayed.
 */
#define SCHEDULE_GRACE_SECONDS 10

/*
 * The main application task must complete a loop cycle
 * and feed the watchdog within this time.
 */
#define SYSTEM_WATCHDOG_TIMEOUT_MS 8000U

/*
 * Simulation Wi-Fi network.
 *
 * The alarm remains fully operational when this network
 * is unavailable.
 */
#define SMART_ALARM_WIFI_SSID "Wokwi-GUEST"
#define SMART_ALARM_WIFI_PASSWORD "" 

#define RTC_MIN_YEAR 2024
#define RTC_MAX_YEAR 2099

/*
 * 1 = generate test bells at +5, +15 and +25 seconds.
 * 0 = use the saved/default school timetable.
 */
#define TEST_MODE 0

#define MAX_TIMETABLE_ENTRIES 80

/* =========================================================
 * NVS configuration
 * ========================================================= */

#define NVS_NAMESPACE "smart_alarm"

#define NVS_AUTO_KEY "auto_enabled"
#define NVS_RING_KEY "ring_ms"

#define NVS_TIMETABLE_VERSION_KEY "tt_version"
#define NVS_TIMETABLE_COUNT_KEY "tt_count"
#define NVS_TIMETABLE_BLOB_KEY "tt_blob"

#define TIMETABLE_STORAGE_VERSION 1U

/* =========================================================
 * LCD configuration
 * ========================================================= */

#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE 0x04
#define LCD_RS 0x01

static const char *TAG = "SMART_ALARM";

/* =========================================================
 * Data structures
 * ========================================================= */

typedef struct
{
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} bell_time_t;

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t date;
    uint8_t weekday;

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_time_t;

typedef enum
{
    RTC_STATUS_OK,
    RTC_STATUS_COMMUNICATION_ERROR,
    RTC_STATUS_OSCILLATOR_STOPPED,
    RTC_STATUS_INVALID_DATA
} rtc_status_t;

typedef enum
{
    CONFIG_CLOSED,

    CONFIG_ADMIN_LOGIN,
    CONFIG_ADMIN_LOCKED,

    CONFIG_CHANGE_PIN_NEW,
    CONFIG_CHANGE_PIN_CONFIRM,

    CONFIG_MENU,
    CONFIG_SET_TIME,
    CONFIG_SET_DATE,
    CONFIG_SET_DURATION,

    CONFIG_TIMETABLE_MENU,
    CONFIG_TIMETABLE_VIEW,
    CONFIG_TIMETABLE_ADD,
    CONFIG_TIMETABLE_DELETE,
    CONFIG_TIMETABLE_CONFIRM_DEFAULT,

    CONFIG_LOG_VIEW,
    CONFIG_LOG_CLEAR_CONFIRM,

    CONFIG_MESSAGE
} config_screen_t;

/* =========================================================
 * Global variables
 * ========================================================= */

static bell_time_t timetable[MAX_TIMETABLE_ENTRIES];

static int timetable_size = 0;
static bool timetable_initialized = false;

static bool auto_enabled = false;

static uint32_t ring_duration_ms =
    DEFAULT_RING_DURATION_MS;

static bool nvs_available = false;
static bool nvs_error = false;

static bool alarm_active = false;
static TickType_t alarm_end_tick;

static uint8_t rtc_stable_readings = 0;
static bool rtc_ready = false;

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t rtc_device;
static i2c_master_dev_handle_t lcd_device;

static int last_trigger_year = -1;
static int last_trigger_month = -1;
static int last_trigger_date = -1;
static int last_trigger_hour = -1;
static int last_trigger_minute = -1;
static int last_trigger_second = -1;

static int last_display_second = -1;

/*
 * Event logging is enabled only after event_log_init()
 * succeeds.
 */
static bool event_log_ready = false;

/*
 * Administrator access control becomes available only
 * after access_control_init() succeeds.
 */
static bool access_control_ready = false;

/*
 * Prevent repeated error messages if feeding the watchdog
 * unexpectedly fails.
 */
static bool watchdog_feed_error_reported = false;

/*
 * HTTP server startup is retried without blocking the
 * alarm when a temporary networking error occurs.
 */
static TickType_t next_web_server_retry_tick = 0;

static bool web_server_start_error_reported =
    false;

/*
 * Local LCD countdown for the 60-second PIN lockout.
 */
static TickType_t admin_lockout_end_tick = 0;
static uint32_t admin_last_remaining_seconds =
    UINT32_MAX;

/*
 * Temporarily stores the first copy of a new administrator
 * PIN while waiting for confirmation.
 */
static char pending_admin_pin[
    ACCESS_PIN_MAX_LENGTH + 1
] = {0};

/*
 * SYSTEM START is recorded only after the RTC has passed
 * its stability checks, so the event receives a valid time.
 */
static bool system_start_logged = false;

/*
 * Captured immediately when app_main starts, then stored
 * after the RTC has supplied a valid timestamp.
 */
static esp_reset_reason_t startup_reset_reason =
    ESP_RST_UNKNOWN;

/*
 * Keep the latest valid RTC reading so events triggered
 * from the keypad or buttons can still receive a timestamp.
 */
static rtc_time_t latest_rtc_time;
static bool latest_rtc_time_valid = false;

/*
 * Used to avoid recording the same RTC fault repeatedly
 * during every loop iteration.
 */
static rtc_status_t previous_rtc_status =
    RTC_STATUS_OK;

/*
 * Protect scheduled ringing against missed loop cycles,
 * RTC jumps and power recovery.
 */
static schedule_guard_t schedule_guard;

static schedule_guard_status_t
    previous_schedule_guard_status =
        SCHEDULE_GUARD_FIRST_READING;

/*
 * After a large jump, scheduled ringing stays blocked
 * for the entire RTC second where the jump was detected.
 */
static int64_t schedule_guard_blocked_second = -1;

/* =========================================================
 * Configuration-menu variables
 * ========================================================= */

static config_screen_t config_screen =
    CONFIG_CLOSED;

static config_screen_t config_message_return_screen =
    CONFIG_MENU;

static char config_input[9];
static size_t config_input_length = 0;

static bool config_display_dirty = false;

static TickType_t config_last_activity_tick = 0;
static TickType_t config_message_end_tick = 0;

static char config_message_line1[21];
static char config_message_line2[21];

static int timetable_selected_index = 0;

/*
 * Offset zero means the newest stored event.
 */
static size_t log_selected_offset = 0;

/* =========================================================
 * Keypad configuration
 * ========================================================= */

static const gpio_num_t keypad_rows[4] = {
    KEYPAD_R1_PIN,
    KEYPAD_R2_PIN,
    KEYPAD_R3_PIN,
    KEYPAD_R4_PIN
};

static const gpio_num_t keypad_columns[4] = {
    KEYPAD_C1_PIN,
    KEYPAD_C2_PIN,
    KEYPAD_C3_PIN,
    KEYPAD_C4_PIN
};

static const char keypad_map[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

/* =========================================================
 * NVS initialization
 * ========================================================= */

static event_log_type_t reset_reason_to_event(
    esp_reset_reason_t reason
)
{
    switch (reason)
    {
        case ESP_RST_POWERON:
            return EVENT_LOG_POWER_ON_RESET;

        case ESP_RST_SW:
            return EVENT_LOG_SOFTWARE_RESET;

        case ESP_RST_PANIC:
            return EVENT_LOG_PANIC_RESET;

        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return EVENT_LOG_WATCHDOG_RESET;

        case ESP_RST_BROWNOUT:
            return EVENT_LOG_BROWNOUT_RESET;

        case ESP_RST_EXT:
            return EVENT_LOG_EXTERNAL_RESET;

        case ESP_RST_DEEPSLEEP:
            return EVENT_LOG_DEEP_SLEEP_WAKE;

        case ESP_RST_UNKNOWN:
        case ESP_RST_SDIO:
        default:
            return EVENT_LOG_UNKNOWN_RESET;
    }
}

static void record_event(
    event_log_type_t type,
    int32_t value
)
{
    if (!event_log_ready)
    {
        return;
    }

    event_log_time_t event_time = {0};
    const event_log_time_t *event_time_pointer = NULL;

    if (latest_rtc_time_valid)
    {
        event_time.year =
            latest_rtc_time.year;

        event_time.month =
            latest_rtc_time.month;

        event_time.date =
            latest_rtc_time.date;

        event_time.weekday =
            latest_rtc_time.weekday;

        event_time.hour =
            latest_rtc_time.hour;

        event_time.minute =
            latest_rtc_time.minute;

        event_time.second =
            latest_rtc_time.second;

        event_time_pointer =
            &event_time;
    }

    esp_err_t result =
        event_log_add(
            type,
            event_time_pointer,
            value
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to record event %s: %s",
            event_log_type_name(type),
            esp_err_to_name(result)
        );
    }
}

/* =========================================================
 * NVS initialization
 * ========================================================= */

static esp_err_t initialize_nvs(void)
{
    esp_err_t result =
        nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(
            TAG,
            "NVS requires reinitialization"
        );

        result =
            nvs_flash_erase();

        if (result != ESP_OK)
        {
            return result;
        }

        result =
            nvs_flash_init();
    }

    return result;
}

/* =========================================================
 * AUTO mode storage
 * ========================================================= */

static esp_err_t save_auto_state(
    bool enabled
)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    result =
        nvs_set_u8(
            handle,
            NVS_AUTO_KEY,
            enabled ? 1U : 0U
        );

    if (result == ESP_OK)
    {
        result =
            nvs_commit(handle);
    }

    nvs_close(handle);

    return result;
}

static esp_err_t load_auto_state(
    bool *enabled
)
{
    if (enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *enabled = false;

    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    uint8_t saved_value = 0;

    result =
        nvs_get_u8(
            handle,
            NVS_AUTO_KEY,
            &saved_value
        );

    if (result == ESP_ERR_NVS_NOT_FOUND ||
        (result == ESP_OK &&
         saved_value > 1U))
    {
        saved_value = 0;

        result =
            nvs_set_u8(
                handle,
                NVS_AUTO_KEY,
                saved_value
            );

        if (result == ESP_OK)
        {
            result =
                nvs_commit(handle);
        }
    }

    if (result == ESP_OK)
    {
        *enabled =
            saved_value == 1U;
    }

    nvs_close(handle);

    return result;
}

static bool apply_auto_state(
    bool enabled
)
{
    auto_enabled = enabled;

    record_event(
        enabled
            ? EVENT_LOG_AUTO_ENABLED
            : EVENT_LOG_AUTO_DISABLED,
        enabled ? 1 : 0
    );

    if (!nvs_available)
    {
        nvs_error = true;
        return false;
    }

    esp_err_t result =
        save_auto_state(enabled);

    nvs_error =
        result != ESP_OK;

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "AUTO state saved: %s",
            enabled ? "ON" : "OFF"
        );

        return true;
    }

    ESP_LOGE(
        TAG,
        "Failed to save AUTO state: %s",
        esp_err_to_name(result)
    );

    return false;
}

/* =========================================================
 * Ring-duration storage
 * ========================================================= */

static esp_err_t save_ring_duration(
    uint32_t duration_ms
)
{
    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
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
            NVS_RING_KEY,
            duration_ms
        );

    if (result == ESP_OK)
    {
        result =
            nvs_commit(handle);
    }

    nvs_close(handle);

    return result;
}

static esp_err_t load_ring_duration(
    uint32_t *duration_ms
)
{
    if (duration_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *duration_ms =
        DEFAULT_RING_DURATION_MS;

    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    uint32_t saved_value =
        DEFAULT_RING_DURATION_MS;

    result =
        nvs_get_u32(
            handle,
            NVS_RING_KEY,
            &saved_value
        );

    bool invalid =
        saved_value <
            MIN_RING_DURATION_SECONDS * 1000U ||
        saved_value >
            MAX_RING_DURATION_SECONDS * 1000U;

    if (result == ESP_ERR_NVS_NOT_FOUND ||
        (result == ESP_OK && invalid))
    {
        saved_value =
            DEFAULT_RING_DURATION_MS;

        result =
            nvs_set_u32(
                handle,
                NVS_RING_KEY,
                saved_value
            );

        if (result == ESP_OK)
        {
            result =
                nvs_commit(handle);
        }
    }

    if (result == ESP_OK)
    {
        *duration_ms = saved_value;
    }

    nvs_close(handle);

    return result;
}

/* =========================================================
 * General utility functions
 * ========================================================= */

static bool is_valid_bcd(
    uint8_t value
)
{
    return
        (value & 0x0F) <= 9U &&
        ((value >> 4) & 0x0F) <= 9U;
}

static uint8_t bcd_to_decimal(
    uint8_t value
)
{
    return (uint8_t)(
        ((value >> 4) * 10U) +
        (value & 0x0F)
    );
}

static uint8_t decimal_to_bcd(
    uint8_t value
)
{
    return (uint8_t)(
        ((value / 10U) << 4) |
        (value % 10U)
    );
}

static bool is_leap_year(
    uint16_t year
)
{
    if (year % 400U == 0U)
    {
        return true;
    }

    if (year % 100U == 0U)
    {
        return false;
    }

    return year % 4U == 0U;
}

static uint8_t days_in_month(
    uint16_t year,
    uint8_t month
)
{
    static const uint8_t month_days[] = {
        0,
        31,
        28,
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31
    };

    if (month < 1U ||
        month > 12U)
    {
        return 0;
    }

    if (month == 2U &&
        is_leap_year(year))
    {
        return 29;
    }

    return month_days[month];
}

static bool rtc_time_is_valid(
    const rtc_time_t *time
)
{
    if (time == NULL)
    {
        return false;
    }

    if (time->year < RTC_MIN_YEAR ||
        time->year > RTC_MAX_YEAR)
    {
        return false;
    }

    if (time->month < 1U ||
        time->month > 12U)
    {
        return false;
    }

    uint8_t maximum_date =
        days_in_month(
            time->year,
            time->month
        );

    if (time->date < 1U ||
        time->date > maximum_date)
    {
        return false;
    }

    if (time->weekday < 1U ||
        time->weekday > 7U)
    {
        return false;
    }

    if (time->hour > 23U ||
        time->minute > 59U ||
        time->second > 59U)
    {
        return false;
    }

    return true;
}

static uint8_t calculate_weekday(
    uint16_t year,
    uint8_t month,
    uint8_t date
)
{
    static const int offsets[] = {
        0,
        3,
        2,
        5,
        0,
        3,
        5,
        1,
        4,
        6,
        2,
        4
    };

    int adjusted_year =
        year;

    if (month < 3U)
    {
        adjusted_year--;
    }

    int weekday =
        (
            adjusted_year +
            adjusted_year / 4 -
            adjusted_year / 100 +
            adjusted_year / 400 +
            offsets[month - 1U] +
            date
        ) % 7;

    return (uint8_t)(
        weekday + 1
    );
}

static int input_to_integer(void)
{
    int value = 0;

    for (size_t i = 0;
         i < config_input_length;
         i++)
    {
        value =
            value * 10 +
            (config_input[i] - '0');
    }

    return value;
}

static const char *weekday_name(
    uint8_t weekday
)
{
    static const char *names[] = {
        "---",
        "Sun",
        "Mon",
        "Tue",
        "Wed",
        "Thu",
        "Fri",
        "Sat"
    };

    if (weekday < 1U ||
        weekday > 7U)
    {
        return "---";
    }

    return names[weekday];
}

static bool is_weekend(
    uint8_t weekday
)
{
    return
        weekday == 1U ||
        weekday == 7U;
}

/* =========================================================
 * GPIO configuration
 * ========================================================= */

static void configure_output_pin(
    gpio_num_t pin
)
{
    gpio_reset_pin(pin);

    gpio_set_direction(
        pin,
        GPIO_MODE_OUTPUT
    );

    gpio_set_level(
        pin,
        0
    );
}

static void configure_button_pin(
    gpio_num_t pin
)
{
    gpio_reset_pin(pin);

    gpio_set_direction(
        pin,
        GPIO_MODE_INPUT
    );

    gpio_set_pull_mode(
        pin,
        GPIO_PULLUP_ONLY
    );
}

/* =========================================================
 * Keypad functions
 * ========================================================= */

static void configure_keypad(void)
{
    for (int row = 0;
         row < 4;
         row++)
    {
        gpio_reset_pin(
            keypad_rows[row]
        );

        gpio_set_direction(
            keypad_rows[row],
            GPIO_MODE_OUTPUT
        );

        gpio_set_level(
            keypad_rows[row],
            1
        );
    }

    for (int column = 0;
         column < 4;
         column++)
    {
        gpio_reset_pin(
            keypad_columns[column]
        );

        gpio_set_direction(
            keypad_columns[column],
            GPIO_MODE_INPUT
        );

        gpio_set_pull_mode(
            keypad_columns[column],
            GPIO_PULLUP_ONLY
        );
    }
}

static char keypad_scan_raw(void)
{
    for (int row = 0;
         row < 4;
         row++)
    {
        for (int other_row = 0;
             other_row < 4;
             other_row++)
        {
            gpio_set_level(
                keypad_rows[other_row],
                1
            );
        }

        gpio_set_level(
            keypad_rows[row],
            0
        );

        esp_rom_delay_us(5);

        for (int column = 0;
             column < 4;
             column++)
        {
            if (gpio_get_level(
                    keypad_columns[column]
                ) == 0)
            {
                for (int restore_row = 0;
                     restore_row < 4;
                     restore_row++)
                {
                    gpio_set_level(
                        keypad_rows[restore_row],
                        1
                    );
                }

                return
                    keypad_map[row][column];
            }
        }
    }

    for (int row = 0;
         row < 4;
         row++)
    {
        gpio_set_level(
            keypad_rows[row],
            1
        );
    }

    return '\0';
}

static char keypad_get_key_event(
    TickType_t now
)
{
    static char last_raw_key = '\0';
    static char reported_key = '\0';
    static TickType_t raw_change_tick = 0;

    char raw_key =
        keypad_scan_raw();

    if (raw_key != last_raw_key)
    {
        last_raw_key = raw_key;
        raw_change_tick = now;
    }

    if (raw_key == '\0')
    {
        reported_key = '\0';
        return '\0';
    }

    if (raw_key != reported_key &&
        now - raw_change_tick >=
            pdMS_TO_TICKS(
                KEYPAD_DEBOUNCE_MS
            ))
    {
        reported_key = raw_key;
        return raw_key;
    }

    return '\0';
}

/* =========================================================
 * I2C configuration
 * ========================================================= */

static void configure_i2c(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    ESP_ERROR_CHECK(
        i2c_new_master_bus(
            &bus_config,
            &i2c_bus
        )
    );

    i2c_device_config_t rtc_config = {
        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,

        .device_address =
            RTC_ADDRESS,

        .scl_speed_hz =
            100000
    };

    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            i2c_bus,
            &rtc_config,
            &rtc_device
        )
    );

    i2c_device_config_t lcd_config = {
        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,

        .device_address =
            LCD_ADDRESS,

        .scl_speed_hz =
            100000
    };

    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            i2c_bus,
            &lcd_config,
            &lcd_device
        )
    );
}

/* =========================================================
 * LCD functions
 * ========================================================= */

static void lcd_expander_write(
    uint8_t value
)
{
    ESP_ERROR_CHECK(
        i2c_master_transmit(
            lcd_device,
            &value,
            1,
            100
        )
    );
}

static void lcd_pulse_enable(
    uint8_t value
)
{
    lcd_expander_write(
        value | LCD_ENABLE
    );

    esp_rom_delay_us(1);

    lcd_expander_write(
        value &
        (uint8_t)~LCD_ENABLE
    );

    esp_rom_delay_us(50);
}

static void lcd_write_nibble(
    uint8_t value,
    bool register_select
)
{
    uint8_t output =
        (value & 0xF0) |
        LCD_BACKLIGHT |
        (
            register_select
            ? LCD_RS
            : 0
        );

    lcd_expander_write(output);
    lcd_pulse_enable(output);
}

static void lcd_send_byte(
    uint8_t value,
    bool register_select
)
{
    lcd_write_nibble(
        value & 0xF0,
        register_select
    );

    lcd_write_nibble(
        (value << 4) & 0xF0,
        register_select
    );

    vTaskDelay(
        pdMS_TO_TICKS(2)
    );
}

static void lcd_command(
    uint8_t command
)
{
    lcd_send_byte(
        command,
        false
    );
}

static void lcd_write_character(
    char character
)
{
    lcd_send_byte(
        (uint8_t)character,
        true
    );
}

static void lcd_initialize(void)
{
    vTaskDelay(
        pdMS_TO_TICKS(50)
    );

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_write_nibble(0x20, false);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x06);
    lcd_command(0x01);

    vTaskDelay(
        pdMS_TO_TICKS(5)
    );
}

static void lcd_set_cursor(
    uint8_t row,
    uint8_t column
)
{
    static const uint8_t row_addresses[] = {
        0x00,
        0x40,
        0x14,
        0x54
    };

    if (row > 3U)
    {
        row = 0;
    }

    lcd_command(
        (uint8_t)(
            0x80 |
            (
                row_addresses[row] +
                column
            )
        )
    );
}

static void lcd_write_line(
    uint8_t row,
    const char *text
)
{
    lcd_set_cursor(row, 0);

    size_t length =
        strlen(text);

    for (int i = 0;
         i < 20;
         i++)
    {
        if ((size_t)i < length)
        {
            lcd_write_character(
                text[i]
            );
        }
        else
        {
            lcd_write_character(' ');
        }
    }
}

/* =========================================================
 * RTC functions
 * ========================================================= */

static rtc_status_t rtc_read_time(
    rtc_time_t *time
)
{
    if (time == NULL)
    {
        return RTC_STATUS_INVALID_DATA;
    }

    uint8_t start_register = 0x00;
    uint8_t data[7];

    esp_err_t result =
        i2c_master_transmit_receive(
            rtc_device,
            &start_register,
            1,
            data,
            sizeof(data),
            100
        );

    if (result != ESP_OK)
    {
        return
            RTC_STATUS_COMMUNICATION_ERROR;
    }

    if ((data[0] & 0x80) != 0)
    {
        return
            RTC_STATUS_OSCILLATOR_STOPPED;
    }

    uint8_t raw_second =
        data[0] & 0x7F;

    uint8_t raw_minute =
        data[1] & 0x7F;

    uint8_t raw_date =
        data[4] & 0x3F;

    uint8_t raw_month =
        data[5] & 0x1F;

    uint8_t raw_year =
        data[6];

    if (!is_valid_bcd(raw_second) ||
        !is_valid_bcd(raw_minute) ||
        !is_valid_bcd(raw_date) ||
        !is_valid_bcd(raw_month) ||
        !is_valid_bcd(raw_year))
    {
        return RTC_STATUS_INVALID_DATA;
    }

    time->second =
        bcd_to_decimal(raw_second);

    time->minute =
        bcd_to_decimal(raw_minute);

    if ((data[2] & 0x40) != 0)
    {
        uint8_t raw_hour =
            data[2] & 0x1F;

        if (!is_valid_bcd(raw_hour))
        {
            return
                RTC_STATUS_INVALID_DATA;
        }

        uint8_t hour =
            bcd_to_decimal(raw_hour);

        bool is_pm =
            (data[2] & 0x20) != 0;

        if (hour < 1U ||
            hour > 12U)
        {
            return
                RTC_STATUS_INVALID_DATA;
        }

        if (is_pm &&
            hour != 12U)
        {
            hour += 12U;
        }

        if (!is_pm &&
            hour == 12U)
        {
            hour = 0;
        }

        time->hour = hour;
    }
    else
    {
        uint8_t raw_hour =
            data[2] & 0x3F;

        if (!is_valid_bcd(raw_hour))
        {
            return
                RTC_STATUS_INVALID_DATA;
        }

        time->hour =
            bcd_to_decimal(raw_hour);
    }

    time->weekday =
        data[3] & 0x07;

    time->date =
        bcd_to_decimal(raw_date);

    time->month =
        bcd_to_decimal(raw_month);

    time->year =
        (uint16_t)(
            2000U +
            bcd_to_decimal(raw_year)
        );

    if (!rtc_time_is_valid(time))
    {
        return RTC_STATUS_INVALID_DATA;
    }

    return RTC_STATUS_OK;
}

static esp_err_t rtc_write_time(
    uint8_t hour,
    uint8_t minute,
    uint8_t second
)
{
    if (hour > 23U ||
        minute > 59U ||
        second > 59U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[] = {
        0x00,
        decimal_to_bcd(second) & 0x7F,
        decimal_to_bcd(minute),
        decimal_to_bcd(hour)
    };

    return
        i2c_master_transmit(
            rtc_device,
            data,
            sizeof(data),
            100
        );
}

static esp_err_t rtc_write_date(
    uint16_t year,
    uint8_t month,
    uint8_t date
)
{
    if (year < RTC_MIN_YEAR ||
        year > RTC_MAX_YEAR ||
        month < 1U ||
        month > 12U ||
        date < 1U ||
        date >
            days_in_month(
                year,
                month
            ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t weekday =
        calculate_weekday(
            year,
            month,
            date
        );

    uint8_t data[] = {
        0x03,
        weekday,
        decimal_to_bcd(date),
        decimal_to_bcd(month),
        decimal_to_bcd(
            (uint8_t)(
                year - 2000U
            )
        )
    };

    return
        i2c_master_transmit(
            rtc_device,
            data,
            sizeof(data),
            100
        );
}

static void reset_schedule_trigger_history(void)
{
    last_trigger_year = -1;
    last_trigger_month = -1;
    last_trigger_date = -1;
    last_trigger_hour = -1;
    last_trigger_minute = -1;
    last_trigger_second = -1;

    last_display_second = -1;
}

static void reset_rtc_validation(void)
{
    rtc_ready = false;
    rtc_stable_readings = 0;

    reset_schedule_trigger_history();

    schedule_guard_init(
        &schedule_guard
    );

    previous_schedule_guard_status =
        SCHEDULE_GUARD_FIRST_READING;

    schedule_guard_blocked_second =
        -1;
}

/* =========================================================
 * Alarm functions
 * ========================================================= */

static void configure_buzzer(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode =
            LEDC_LOW_SPEED_MODE,

        .duty_resolution =
            LEDC_TIMER_10_BIT,

        .timer_num =
            LEDC_TIMER_0,

        .freq_hz =
            2000,

        .clk_cfg =
            LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(
        ledc_timer_config(
            &timer_config
        )
    );

    ledc_channel_config_t channel_config = {
        .gpio_num =
            BUZZER_PIN,

        .speed_mode =
            LEDC_LOW_SPEED_MODE,

        .channel =
            LEDC_CHANNEL_0,

        .intr_type =
            LEDC_INTR_DISABLE,

        .timer_sel =
            LEDC_TIMER_0,

        .duty = 0,
        .hpoint = 0
    };

    ESP_ERROR_CHECK(
        ledc_channel_config(
            &channel_config
        )
    );
}

static void set_alarm_output(
    bool enabled
)
{
    gpio_set_level(
        LED_PIN,
        enabled ? 1 : 0
    );

    gpio_set_level(
        RELAY_PIN,
        enabled ? 1 : 0
    );

    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0,
        enabled ? 512U : 0U
    );

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0
    );
}

static void start_alarm(void)
{
    /*
     * Emergency and live announcements have higher priority
     * than scheduled, manual and test bells.
     */
    if (announcement_blocks_bell())
    {
        ESP_LOGW(
            TAG,
            "Bell start blocked by announcement priority"
        );

        return;
    }

    alarm_active = true;

    alarm_end_tick =
        xTaskGetTickCount() +
        pdMS_TO_TICKS(
            ring_duration_ms
        );

    set_alarm_output(true);
}

static void stop_alarm(void)
{
    alarm_active = false;
    set_alarm_output(false);
}

/* =========================================================
 * Timetable validation
 * ========================================================= */

static bool timetable_entry_is_valid(
    const bell_time_t *entry
)
{
    if (entry == NULL)
    {
        return false;
    }

    if (entry->weekday < 1U ||
        entry->weekday > 7U)
    {
        return false;
    }

    if (entry->hour > 23U ||
        entry->minute > 59U ||
        entry->second > 59U)
    {
        return false;
    }

    return true;
}

static int timetable_entry_seconds(
    const bell_time_t *entry
)
{
    return
        (entry->weekday - 1) * 86400 +
        entry->hour * 3600 +
        entry->minute * 60 +
        entry->second;
}

static void sort_timetable(
    bell_time_t *entries,
    int count
)
{
    for (int i = 1;
         i < count;
         i++)
    {
        bell_time_t current =
            entries[i];

        int current_seconds =
            timetable_entry_seconds(
                &current
            );

        int position =
            i - 1;

        while (
            position >= 0 &&
            timetable_entry_seconds(
                &entries[position]
            ) > current_seconds
        )
        {
            entries[position + 1] =
                entries[position];

            position--;
        }

        entries[position + 1] =
            current;
    }
}

static bool timetable_has_duplicate(
    const bell_time_t *entries,
    int count
)
{
    for (int i = 0;
         i < count;
         i++)
    {
        for (int j = i + 1;
             j < count;
             j++)
        {
            if (
                entries[i].weekday ==
                    entries[j].weekday &&

                entries[i].hour ==
                    entries[j].hour &&

                entries[i].minute ==
                    entries[j].minute &&

                entries[i].second ==
                    entries[j].second
            )
            {
                return true;
            }
        }
    }

    return false;
}

static bool timetable_contains_entry(
    const bell_time_t *entry
)
{
    for (int i = 0;
         i < timetable_size;
         i++)
    {
        if (
            timetable[i].weekday ==
                entry->weekday &&

            timetable[i].hour ==
                entry->hour &&

            timetable[i].minute ==
                entry->minute &&

            timetable[i].second ==
                entry->second
        )
        {
            return true;
        }
    }

    return false;
}

static bool timetable_is_valid(
    const bell_time_t *entries,
    int count
)
{
    if (entries == NULL)
    {
        return false;
    }

    if (count <= 0 ||
        count >
            MAX_TIMETABLE_ENTRIES)
    {
        return false;
    }

    for (int i = 0;
         i < count;
         i++)
    {
        if (!timetable_entry_is_valid(
                &entries[i]
            ))
        {
            return false;
        }
    }

    if (timetable_has_duplicate(
            entries,
            count
        ))
    {
        return false;
    }

    return true;
}

/* =========================================================
 * Default timetable
 * ========================================================= */

static void add_default_entry(
    uint8_t weekday,
    uint8_t hour,
    uint8_t minute
)
{
    if (timetable_size >=
        MAX_TIMETABLE_ENTRIES)
    {
        return;
    }

    timetable[timetable_size] =
        (bell_time_t){
            .weekday = weekday,
            .hour = hour,
            .minute = minute,
            .second = 0
        };

    timetable_size++;
}

static void load_default_timetable(void)
{
    timetable_size = 0;

    for (uint8_t day = 2;
         day <= 6;
         day++)
    {
        add_default_entry(day, 8, 0);
        add_default_entry(day, 8, 40);
        add_default_entry(day, 9, 20);

        add_default_entry(day, 10, 0);
        add_default_entry(day, 10, 15);

        add_default_entry(day, 10, 55);
        add_default_entry(day, 11, 35);
        add_default_entry(day, 12, 15);

        add_default_entry(day, 12, 30);

        add_default_entry(day, 14, 0);
        add_default_entry(day, 14, 40);
        add_default_entry(day, 15, 20);
        add_default_entry(day, 16, 0);
        add_default_entry(day, 16, 40);

        add_default_entry(day, 17, 20);
    }

    sort_timetable(
        timetable,
        timetable_size
    );

    timetable_initialized = true;

    reset_schedule_trigger_history();

    ESP_LOGI(
        TAG,
        "Default timetable loaded: %d entries",
        timetable_size
    );
}

/* =========================================================
 * Timetable NVS storage
 * ========================================================= */

static esp_err_t read_timetable_from_nvs(
    bell_time_t *entries,
    uint16_t *entry_count
)
{
    if (entries == NULL ||
        entry_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (result != ESP_OK)
    {
        return result;
    }

    uint32_t saved_version = 0;
    uint16_t saved_count = 0;
    size_t blob_size = 0;

    result =
        nvs_get_u32(
            handle,
            NVS_TIMETABLE_VERSION_KEY,
            &saved_version
        );

    if (result != ESP_OK)
    {
        nvs_close(handle);
        return result;
    }

    if (saved_version !=
        TIMETABLE_STORAGE_VERSION)
    {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    result =
        nvs_get_u16(
            handle,
            NVS_TIMETABLE_COUNT_KEY,
            &saved_count
        );

    if (result != ESP_OK)
    {
        nvs_close(handle);
        return result;
    }

    if (saved_count == 0U ||
        saved_count >
            MAX_TIMETABLE_ENTRIES)
    {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    result =
        nvs_get_blob(
            handle,
            NVS_TIMETABLE_BLOB_KEY,
            NULL,
            &blob_size
        );

    if (result != ESP_OK)
    {
        nvs_close(handle);
        return result;
    }

    size_t expected_size =
        (size_t)saved_count *
        sizeof(bell_time_t);

    if (blob_size != expected_size)
    {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    result =
        nvs_get_blob(
            handle,
            NVS_TIMETABLE_BLOB_KEY,
            entries,
            &blob_size
        );

    nvs_close(handle);

    if (result == ESP_OK)
    {
        *entry_count =
            saved_count;
    }

    return result;
}

static esp_err_t verify_saved_timetable(void)
{
    bell_time_t verified_entries[
        MAX_TIMETABLE_ENTRIES
    ];

    uint16_t verified_count = 0;

    esp_err_t result =
        read_timetable_from_nvs(
            verified_entries,
            &verified_count
        );

    if (result != ESP_OK)
    {
        return result;
    }

    if (!timetable_is_valid(
            verified_entries,
            verified_count
        ))
    {
        return ESP_ERR_INVALID_STATE;
    }

    sort_timetable(
        verified_entries,
        verified_count
    );

    if ((int)verified_count !=
        timetable_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(
            verified_entries,
            timetable,
            timetable_size *
                sizeof(bell_time_t)
        ) != 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t save_timetable_to_nvs(void)
{
    if (!nvs_available)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!timetable_is_valid(
            timetable,
            timetable_size
        ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    sort_timetable(
        timetable,
        timetable_size
    );

    nvs_handle_t handle;

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
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
            NVS_TIMETABLE_VERSION_KEY,
            TIMETABLE_STORAGE_VERSION
        );

    if (result == ESP_OK)
    {
        result =
            nvs_set_u16(
                handle,
                NVS_TIMETABLE_COUNT_KEY,
                (uint16_t)
                    timetable_size
            );
    }

    if (result == ESP_OK)
    {
        result =
            nvs_set_blob(
                handle,
                NVS_TIMETABLE_BLOB_KEY,
                timetable,
                timetable_size *
                    sizeof(bell_time_t)
            );
    }

    if (result == ESP_OK)
    {
        result =
            nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK)
    {
        return result;
    }

    return verify_saved_timetable();
}

static esp_err_t load_timetable_from_nvs(void)
{
    bell_time_t loaded_entries[
        MAX_TIMETABLE_ENTRIES
    ];

    uint16_t loaded_count = 0;

    esp_err_t result =
        read_timetable_from_nvs(
            loaded_entries,
            &loaded_count
        );

    if (result != ESP_OK)
    {
        return result;
    }

    if (!timetable_is_valid(
            loaded_entries,
            loaded_count
        ))
    {
        return ESP_ERR_INVALID_STATE;
    }

    sort_timetable(
        loaded_entries,
        loaded_count
    );

    memcpy(
        timetable,
        loaded_entries,
        loaded_count *
            sizeof(bell_time_t)
    );

    timetable_size =
        loaded_count;

    timetable_initialized = true;

    reset_schedule_trigger_history();

    ESP_LOGI(
        TAG,
        "Saved timetable loaded: %d entries",
        timetable_size
    );

    return ESP_OK;
}

static void initialize_timetable_storage(void)
{
    if (!nvs_available)
    {
        load_default_timetable();
        nvs_error = true;
        return;
    }

    esp_err_t result =
        load_timetable_from_nvs();

    if (result == ESP_OK)
    {
        return;
    }

    ESP_LOGW(
        TAG,
        "No valid timetable in NVS"
    );

    load_default_timetable();

    result =
        save_timetable_to_nvs();

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Default timetable saved and verified"
        );
    }
    else
    {
        nvs_error = true;

        ESP_LOGE(
            TAG,
            "Failed to save default timetable: %s",
            esp_err_to_name(result)
        );
    }
}

/* =========================================================
 * Timetable editing
 * ========================================================= */

static void restore_timetable_backup(
    const bell_time_t *backup,
    int backup_count
)
{
    memcpy(
        timetable,
        backup,
        backup_count *
            sizeof(bell_time_t)
    );

    timetable_size =
        backup_count;

    timetable_initialized =
        true;
}

static esp_err_t add_timetable_bell(
    const bell_time_t *new_entry
)
{
    if (!timetable_entry_is_valid(
            new_entry
        ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (timetable_size >=
        MAX_TIMETABLE_ENTRIES)
    {
        return ESP_ERR_NO_MEM;
    }

    if (timetable_contains_entry(
            new_entry
        ))
    {
        return ESP_ERR_INVALID_STATE;
    }

    bell_time_t backup[
        MAX_TIMETABLE_ENTRIES
    ];

    int backup_count =
        timetable_size;

    memcpy(
        backup,
        timetable,
        timetable_size *
            sizeof(bell_time_t)
    );

    timetable[timetable_size] =
        *new_entry;

    timetable_size++;

    sort_timetable(
        timetable,
        timetable_size
    );

    esp_err_t result =
        save_timetable_to_nvs();

    if (result != ESP_OK)
    {
        restore_timetable_backup(
            backup,
            backup_count
        );

        return result;
    }

    reset_schedule_trigger_history();

    return ESP_OK;
}

static esp_err_t delete_timetable_bell(
    int index
)
{
    if (index < 0 ||
        index >= timetable_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Never allow an empty timetable.
     */
    if (timetable_size <= 1)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bell_time_t backup[
        MAX_TIMETABLE_ENTRIES
    ];

    int backup_count =
        timetable_size;

    memcpy(
        backup,
        timetable,
        timetable_size *
            sizeof(bell_time_t)
    );

    for (int i = index;
         i < timetable_size - 1;
         i++)
    {
        timetable[i] =
            timetable[i + 1];
    }

    timetable_size--;

    esp_err_t result =
        save_timetable_to_nvs();

    if (result != ESP_OK)
    {
        restore_timetable_backup(
            backup,
            backup_count
        );

        return result;
    }

    reset_schedule_trigger_history();

    return ESP_OK;
}

static esp_err_t restore_default_timetable(void)
{
    bell_time_t backup[
        MAX_TIMETABLE_ENTRIES
    ];

    int backup_count =
        timetable_size;

    memcpy(
        backup,
        timetable,
        timetable_size *
            sizeof(bell_time_t)
    );

    load_default_timetable();

    esp_err_t result =
        save_timetable_to_nvs();

    if (result != ESP_OK)
    {
        restore_timetable_backup(
            backup,
            backup_count
        );

        return result;
    }

    reset_schedule_trigger_history();

    return ESP_OK;
}

/* =========================================================
 * TEST_MODE timetable
 * ========================================================= */

#if TEST_MODE

static bell_time_t add_seconds_to_time(
    const rtc_time_t *current_time,
    int seconds_to_add
)
{
    int total_seconds =
        current_time->hour * 3600 +
        current_time->minute * 60 +
        current_time->second +
        seconds_to_add;

    int added_days =
        total_seconds / 86400;

    total_seconds %= 86400;

    bell_time_t result = {
        .weekday =
            (uint8_t)(
                (
                    current_time->weekday -
                    1 +
                    added_days
                ) % 7 + 1
            ),

        .hour =
            (uint8_t)(
                total_seconds / 3600
            ),

        .minute =
            (uint8_t)(
                (total_seconds % 3600) /
                60
            ),

        .second =
            (uint8_t)(
                total_seconds % 60
            )
    };

    return result;
}

#endif

static void initialize_runtime_timetable(
    const rtc_time_t *current_time
)
{
#if TEST_MODE

    timetable[0] =
        add_seconds_to_time(
            current_time,
            5
        );

    timetable[1] =
        add_seconds_to_time(
            current_time,
            15
        );

    timetable[2] =
        add_seconds_to_time(
            current_time,
            25
        );

    timetable_size = 3;
    timetable_initialized = true;

#else

    (void)current_time;

    if (!timetable_initialized)
    {
        load_default_timetable();
    }

#endif
}

/* =========================================================
 * Timetable runtime
 * ========================================================= */

static schedule_clock_t rtc_to_schedule_clock(
    const rtc_time_t *time
)
{
    schedule_clock_t result = {
        .year = time->year,
        .month = time->month,
        .date = time->date,
        .hour = time->hour,
        .minute = time->minute,
        .second = time->second
    };

    return result;
}

/*
 * Check whether the RTC crossed a bell time during
 * a small accepted delay.
 */
static bool timetable_crossed_during_guard(
    const rtc_time_t *current_time
)
{
    if (current_time == NULL)
    {
        return false;
    }

    for (int i = 0;
         i < timetable_size;
         i++)
    {
        /*
         * Only create candidates belonging to the
         * current RTC weekday.
         */
        if (timetable[i].weekday !=
            current_time->weekday)
        {
            continue;
        }

        schedule_clock_t bell_clock = {
            .year = current_time->year,
            .month = current_time->month,
            .date = current_time->date,

            .hour = timetable[i].hour,
            .minute = timetable[i].minute,
            .second = timetable[i].second
        };

        if (schedule_guard_crossed_time(
                &schedule_guard,
                &bell_clock
            ))
        {
            return true;
        }
    }

    return false;
}

static bool timetable_matches(
    const rtc_time_t *time
)
{
    for (int i = 0;
         i < timetable_size;
         i++)
    {
        if (
            timetable[i].weekday ==
                time->weekday &&

            timetable[i].hour ==
                time->hour &&

            timetable[i].minute ==
                time->minute &&

            timetable[i].second ==
                time->second
        )
        {
            return true;
        }
    }

    return false;
}

static bool get_next_bell(
    const rtc_time_t *time,
    bell_time_t *next_bell
)
{
    if (time == NULL ||
        next_bell == NULL ||
        timetable_size <= 0)
    {
        return false;
    }

    int current_week_seconds =
        (time->weekday - 1) * 86400 +
        time->hour * 3600 +
        time->minute * 60 +
        time->second;

    int smallest_difference =
        (7 * 86400) + 1;

    for (int i = 0;
         i < timetable_size;
         i++)
    {
        int bell_week_seconds =
            (
                timetable[i].weekday -
                1
            ) * 86400 +
            timetable[i].hour * 3600 +
            timetable[i].minute * 60 +
            timetable[i].second;

        int difference =
            bell_week_seconds -
            current_week_seconds;

        if (difference <= 0)
        {
            difference +=
                7 * 86400;
        }

        if (difference <
            smallest_difference)
        {
            smallest_difference =
                difference;

            *next_bell =
                timetable[i];
        }
    }

    return true;
}

/* =========================================================
 * Normal display functions
 * ========================================================= */

static void display_rtc_fault(
    rtc_status_t status
)
{
    lcd_write_line(
        0,
        "SMART SCHOOL ALARM"
    );

    if (status ==
        RTC_STATUS_COMMUNICATION_ERROR)
    {
        lcd_write_line(
            1,
            "RTC CONNECTION ERROR"
        );

        lcd_write_line(
            2,
            "Check I2C wiring"
        );
    }
    else if (status ==
             RTC_STATUS_OSCILLATOR_STOPPED)
    {
        lcd_write_line(
            1,
            "RTC OSCILLATOR STOP"
        );

        lcd_write_line(
            2,
            "Press A to set RTC"
        );
    }
    else
    {
        lcd_write_line(
            1,
            "RTC TIME INVALID"
        );

        lcd_write_line(
            2,
            "Press A to set RTC"
        );
    }

    lcd_write_line(
        3,
        "Relay forced OFF"
    );
}

static void display_rtc_recovering(void)
{
    char line[21];

    lcd_write_line(
        0,
        "SMART SCHOOL ALARM"
    );

    lcd_write_line(
        1,
        "RTC RECOVERING"
    );

    snprintf(
        line,
        sizeof(line),
        "Stable reads: %u/%u",
        rtc_stable_readings,
        RTC_STABLE_READINGS_REQUIRED
    );

    lcd_write_line(2, line);

    lcd_write_line(
        3,
        "Relay forced OFF"
    );
}

static void update_display(
    const rtc_time_t *time
)
{
    char line[21];
    bell_time_t next_bell;

    /*
     * Announcements temporarily replace the normal LCD screen.
     * Emergency and live modes have priority over bell details.
     */
    announcement_status_t announcement_status;

    if (
        announcement_get_status(
            &announcement_status
        ) == ESP_OK
    )
    {
        if (
            announcement_status.state ==
            ANNOUNCEMENT_STATE_EMERGENCY
        )
        {
            lcd_write_line(
                0,
                "!!! EMERGENCY !!!"
            );

            lcd_write_line(
                1,
                "ANNOUNCEMENT ACTIVE"
            );

            lcd_write_line(
                2,
                "BELL OUTPUT: BLOCKED"
            );

            lcd_write_line(
                3,
                "PRESS E TO CANCEL"
            );

            return;
        }

        if (
            announcement_status.state ==
            ANNOUNCEMENT_STATE_LIVE
        )
        {
            lcd_write_line(
                0,
                "LIVE ANNOUNCEMENT"
            );

            lcd_write_line(
                1,
                "DIRECTOR SPEAKING"
            );

            lcd_write_line(
                2,
                "BELL OUTPUT: BLOCKED"
            );

            lcd_write_line(
                3,
                "RELEASE PTT TO STOP"
            );

            return;
        }
    }

    lcd_write_line(
        0,
        "SMART SCHOOL ALARM"
    );

    snprintf(
        line,
        sizeof(line),
        "%s Time:%02u:%02u:%02u",
        weekday_name(
            time->weekday
        ),
        time->hour,
        time->minute,
        time->second
    );

    lcd_write_line(1, line);

    if (get_next_bell(
            time,
            &next_bell
        ))
    {
        snprintf(
            line,
            sizeof(line),
            "Next %s %02u:%02u:%02u",
            weekday_name(
                next_bell.weekday
            ),
            next_bell.hour,
            next_bell.minute,
            next_bell.second
        );
    }
    else
    {
        snprintf(
            line,
            sizeof(line),
            "No timetable"
        );
    }

    lcd_write_line(2, line);

    if (alarm_active)
    {
        snprintf(
            line,
            sizeof(line),
            "RINGING AUTO:%s",
            auto_enabled
                ? "ON"
                : "OFF"
        );
    }
    else if (nvs_error)
    {
        snprintf(
            line,
            sizeof(line),
            "NVS ERROR AUTO:%s",
            auto_enabled
                ? "ON"
                : "OFF"
        );
    }
    else if (is_weekend(
                 time->weekday
             ))
    {
        snprintf(
            line,
            sizeof(line),
            "WEEKEND AUTO:%s",
            auto_enabled
                ? "ON"
                : "OFF"
        );
    }
    else
    {
        snprintf(
            line,
            sizeof(line),
            "READY AUTO:%s",
            auto_enabled
                ? "ON"
                : "OFF"
        );
    }

    lcd_write_line(3, line);
}

/* =========================================================
 * Configuration helpers
 * ========================================================= */

static void clear_config_input(void)
{
    memset(
        config_input,
        0,
        sizeof(config_input)
    );

    config_input_length = 0;
}

static void begin_admin_login(
    TickType_t now
)
{
    /*
     * Configuration access must never leave the bell active.
     */
    stop_alarm();

    clear_config_input();

    config_screen =
        CONFIG_ADMIN_LOGIN;

    config_last_activity_tick =
        now;

    config_display_dirty =
        true;
}

static void open_config_menu(
    TickType_t now
)
{
    stop_alarm();

    clear_config_input();

    config_screen =
        CONFIG_MENU;

    config_last_activity_tick =
        now;

    config_display_dirty =
        true;
}

static void close_config_menu(void)
{
    clear_config_input();

    config_screen =
        CONFIG_CLOSED;

    config_display_dirty =
        false;

    last_display_second =
        -1;
}

static void show_config_message(
    const char *line1,
    const char *line2,
    config_screen_t return_screen,
    TickType_t now
)
{
    snprintf(
        config_message_line1,
        sizeof(config_message_line1),
        "%s",
        line1
    );

    snprintf(
        config_message_line2,
        sizeof(config_message_line2),
        "%s",
        line2
    );

    config_message_return_screen =
        return_screen;

    config_screen =
        CONFIG_MESSAGE;

    config_message_end_tick =
        now +
        pdMS_TO_TICKS(
            CONFIG_MESSAGE_MS
        );

    config_last_activity_tick =
        now;

    config_display_dirty =
        true;
}

static void append_config_digit(
    char digit
)
{
    size_t maximum_length = 0;

    if (
        config_screen ==
            CONFIG_ADMIN_LOGIN ||
        config_screen ==
            CONFIG_CHANGE_PIN_NEW ||
        config_screen ==
            CONFIG_CHANGE_PIN_CONFIRM
    )
    {
        maximum_length =
            ACCESS_PIN_MAX_LENGTH;
    }
    else if (config_screen ==
             CONFIG_SET_TIME)
    {
        maximum_length = 4;
    }
    else if (config_screen ==
             CONFIG_SET_DATE)
    {
        maximum_length = 8;
    }
    else if (config_screen ==
             CONFIG_SET_DURATION)
    {
        maximum_length = 2;
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_ADD)
    {
        maximum_length = 5;
    }

    if (config_input_length <
        maximum_length)
    {
        config_input[
            config_input_length
        ] = digit;

        config_input_length++;

        config_input[
            config_input_length
        ] = '\0';

        config_display_dirty =
            true;
    }
}

static void delete_config_digit(void)
{
    if (config_input_length > 0)
    {
        config_input_length--;

        config_input[
            config_input_length
        ] = '\0';

        config_display_dirty =
            true;
    }
}

static void select_previous_timetable_entry(void)
{
    if (timetable_size <= 0)
    {
        timetable_selected_index = 0;
        return;
    }

    timetable_selected_index--;

    if (timetable_selected_index < 0)
    {
        timetable_selected_index =
            timetable_size - 1;
    }

    config_display_dirty = true;
}

static void select_next_timetable_entry(void)
{
    if (timetable_size <= 0)
    {
        timetable_selected_index = 0;
        return;
    }

    timetable_selected_index++;

    if (timetable_selected_index >=
        timetable_size)
    {
        timetable_selected_index = 0;
    }

    config_display_dirty = true;
}

/* =========================================================
 * Event-log viewer helpers
 * ========================================================= */

static void select_newer_log_event(void)
{
    if (log_selected_offset > 0)
    {
        log_selected_offset--;

        config_display_dirty =
            true;
    }
}

static void select_older_log_event(void)
{
    size_t total_logs =
        event_log_count();

    if (total_logs == 0)
    {
        log_selected_offset = 0;
        return;
    }

    if ((log_selected_offset + 1U) <
        total_logs)
    {
        log_selected_offset++;

        config_display_dirty =
            true;
    }
}

static void write_two_digits(
    char *destination,
    uint8_t value
)
{
    destination[0] =
        (char)(
            '0' +
            ((value / 10U) % 10U)
        );

    destination[1] =
        (char)(
            '0' +
            (value % 10U)
        );
}

static void format_event_timestamp(
    const event_log_record_t *record,
    char *output,
    size_t output_size
)
{
    if (record == NULL ||
        output == NULL ||
        output_size < 18U)
    {
        return;
    }

    if (record->time.year == 0U)
    {
        snprintf(
            output,
            output_size,
            "Time unavailable"
        );

        return;
    }

    uint8_t short_year =
        (uint8_t)(
            record->time.year %
            100U
        );

    /*
     * Format:
     *
     * DD/MM/YY HH:MM:SS
     */
    write_two_digits(
        &output[0],
        record->time.date
    );

    output[2] = '/';

    write_two_digits(
        &output[3],
        record->time.month
    );

    output[5] = '/';

    write_two_digits(
        &output[6],
        short_year
    );

    output[8] = ' ';

    write_two_digits(
        &output[9],
        record->time.hour
    );

    output[11] = ':';

    write_two_digits(
        &output[12],
        record->time.minute
    );

    output[14] = ':';

    write_two_digits(
        &output[15],
        record->time.second
    );

    output[17] = '\0';
}

/* =========================================================
 * Configuration input formatting
 * ========================================================= */

static void format_time_input(
    char *output,
    size_t output_size
)
{
    char characters[4] = {
        '_',
        '_',
        '_',
        '_'
    };

    for (size_t i = 0;
         i < config_input_length &&
         i < 4;
         i++)
    {
        characters[i] =
            config_input[i];
    }

    snprintf(
        output,
        output_size,
        "%c%c:%c%c",
        characters[0],
        characters[1],
        characters[2],
        characters[3]
    );
}

static void format_date_input(
    char *output,
    size_t output_size
)
{
    char characters[8] = {
        '_',
        '_',
        '_',
        '_',
        '_',
        '_',
        '_',
        '_'
    };

    for (size_t i = 0;
         i < config_input_length &&
         i < 8;
         i++)
    {
        characters[i] =
            config_input[i];
    }

    snprintf(
        output,
        output_size,
        "%c%c/%c%c/%c%c%c%c",
        characters[0],
        characters[1],
        characters[2],
        characters[3],
        characters[4],
        characters[5],
        characters[6],
        characters[7]
    );
}

static void format_bell_input(
    char *output,
    size_t output_size
)
{
    char characters[5] = {
        '_',
        '_',
        '_',
        '_',
        '_'
    };

    for (size_t i = 0;
         i < config_input_length &&
         i < 5;
         i++)
    {
        characters[i] =
            config_input[i];
    }

    snprintf(
        output,
        output_size,
        "Day %c  %c%c:%c%c",
        characters[0],
        characters[1],
        characters[2],
        characters[3],
        characters[4]
    );
}

/* =========================================================
 * Configuration screen display
 * ========================================================= */

static void display_config_screen(void)
{
    char line[21];

    if (config_screen ==
        CONFIG_ADMIN_LOGIN)
    {
        char hidden_pin[
            ACCESS_PIN_MAX_LENGTH + 1
        ];

        size_t hidden_length =
            config_input_length;

        if (hidden_length >
            ACCESS_PIN_MAX_LENGTH)
        {
            hidden_length =
                ACCESS_PIN_MAX_LENGTH;
        }

        for (size_t i = 0;
             i < hidden_length;
             i++)
        {
            hidden_pin[i] = '*';
        }

        hidden_pin[
            hidden_length
        ] = '\0';

        lcd_write_line(
            0,
            "ADMIN LOGIN"
        );

        snprintf(
            line,
            sizeof(line),
            "PIN: %s",
            hidden_pin
        );

        lcd_write_line(
            1,
            line
        );

        lcd_write_line(
            2,
            "# CONFIRM * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_ADMIN_LOCKED)
    {
        TickType_t current_tick =
            xTaskGetTickCount();

        uint32_t remaining_seconds =
            0;

        if ((int32_t)(
                admin_lockout_end_tick -
                current_tick
            ) > 0)
        {
            uint32_t remaining_ms =
                (uint32_t)(
                    admin_lockout_end_tick -
                    current_tick
                ) *
                portTICK_PERIOD_MS;

            remaining_seconds =
                (
                    remaining_ms +
                    999U
                ) /
                1000U;
        }

        lcd_write_line(
            0,
            "ACCESS LOCKED"
        );

        lcd_write_line(
            1,
            "Too many attempts"
        );

        snprintf(
            line,
            sizeof(line),
            "Retry in %lus",
            (unsigned long)
                remaining_seconds
        );

        lcd_write_line(
            2,
            line
        );

        lcd_write_line(
            3,
            "D EXIT"
        );
    }
    else if (config_screen ==
             CONFIG_CHANGE_PIN_NEW)
    {
        char hidden_pin[
            ACCESS_PIN_MAX_LENGTH + 1
        ];

        size_t hidden_length =
            config_input_length;

        if (hidden_length >
            ACCESS_PIN_MAX_LENGTH)
        {
            hidden_length =
                ACCESS_PIN_MAX_LENGTH;
        }

        for (size_t i = 0;
             i < hidden_length;
             i++)
        {
            hidden_pin[i] = '*';
        }

        hidden_pin[hidden_length] =
            '\0';

        lcd_write_line(
            0,
            "CHANGE ADMIN PIN"
        );

        snprintf(
            line,
            sizeof(line),
            "New PIN: %s",
            hidden_pin
        );

        lcd_write_line(
            1,
            line
        );

        lcd_write_line(
            2,
            "# NEXT  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_CHANGE_PIN_CONFIRM)
    {
        char hidden_pin[
            ACCESS_PIN_MAX_LENGTH + 1
        ];

        size_t hidden_length =
            config_input_length;

        if (hidden_length >
            ACCESS_PIN_MAX_LENGTH)
        {
            hidden_length =
                ACCESS_PIN_MAX_LENGTH;
        }

        for (size_t i = 0;
             i < hidden_length;
             i++)
        {
            hidden_pin[i] = '*';
        }

        hidden_pin[hidden_length] =
            '\0';

        lcd_write_line(
            0,
            "CONFIRM NEW PIN"
        );

        snprintf(
            line,
            sizeof(line),
            "PIN: %s",
            hidden_pin
        );

        lcd_write_line(
            1,
            line
        );

        lcd_write_line(
            2,
            "# SAVE  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_MENU)
    {
        lcd_write_line(
            0,
            "CONFIGURATION MENU"
        );

        lcd_write_line(
            1,
            "1:TIME  2:DATE"
        );

        lcd_write_line(
            2,
            "3:DURATION 4:TEST"
        );

        lcd_write_line(
            3,
            "5AUTO 6BEL 7LOG 8PIN"
        );
    }
    else if (config_screen ==
             CONFIG_SET_TIME)
    {
        format_time_input(
            line,
            sizeof(line)
        );

        lcd_write_line(
            0,
            "SET TIME - HHMM"
        );

        lcd_write_line(1, line);

        lcd_write_line(
            2,
            "# SAVE  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_SET_DATE)
    {
        format_date_input(
            line,
            sizeof(line)
        );

        lcd_write_line(
            0,
            "SET DATE DDMMYYYY"
        );

        lcd_write_line(1, line);

        lcd_write_line(
            2,
            "# SAVE  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_SET_DURATION)
    {
        if (config_input_length == 0)
        {
            snprintf(
                line,
                sizeof(line),
                "Seconds: __"
            );
        }
        else
        {
            snprintf(
                line,
                sizeof(line),
                "Seconds: %s",
                config_input
            );
        }

        lcd_write_line(
            0,
            "BELL DURATION 1-60"
        );

        lcd_write_line(1, line);

        lcd_write_line(
            2,
            "# SAVE  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_MENU)
    {
        lcd_write_line(
            0,
            "TIMETABLE MENU"
        );

        lcd_write_line(
            1,
            "1:VIEW    2:ADD"
        );

        lcd_write_line(
            2,
            "3:DELETE  4:DEFAULT"
        );

        lcd_write_line(
            3,
            "D:BACK"
        );
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_VIEW)
    {
        if (timetable_size <= 0)
        {
            lcd_write_line(
                0,
                "NO TIMETABLE"
            );

            lcd_write_line(
                1,
                ""
            );
        }
        else
        {
            bell_time_t entry =
                timetable[
                    timetable_selected_index
                ];

            snprintf(
                line,
                sizeof(line),
                "BELL %02hhu/%02hhu",
                timetable_selected_index + 1,
                timetable_size
            );

            lcd_write_line(0, line);

            snprintf(
                line,
                sizeof(line),
                "%s %02u:%02u:%02u",
                weekday_name(
                    entry.weekday
                ),
                entry.hour,
                entry.minute,
                entry.second
            );

            lcd_write_line(1, line);
        }

        lcd_write_line(
            2,
            "A:PREV  B:NEXT"
        );

        lcd_write_line(
            3,
            "D:BACK"
        );
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_ADD)
    {
        format_bell_input(
            line,
            sizeof(line)
        );

        lcd_write_line(
            0,
            "ADD BELL - DHHMM"
        );

        lcd_write_line(1, line);

        lcd_write_line(
            2,
            "# SAVE  * DELETE"
        );

        lcd_write_line(
            3,
            "D CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_DELETE)
    {
        bell_time_t entry =
            timetable[
                timetable_selected_index
            ];

        snprintf(
            line,
            sizeof(line),
            "DELETE BELL %02hhu/%02hhu",
            timetable_selected_index + 1,
            timetable_size
        );

        lcd_write_line(0, line);

        snprintf(
            line,
            sizeof(line),
            "%s %02u:%02u:%02u",
            weekday_name(
                entry.weekday
            ),
            entry.hour,
            entry.minute,
            entry.second
        );

        lcd_write_line(1, line);

        lcd_write_line(
            2,
            "A/B SELECT #DELETE"
        );

        lcd_write_line(
            3,
            "D:CANCEL"
        );
    }
    else if (config_screen ==
             CONFIG_TIMETABLE_CONFIRM_DEFAULT)
    {
        lcd_write_line(
            0,
            "RESTORE DEFAULTS?"
        );

        lcd_write_line(
            1,
            "Replaces all bells"
        );

        lcd_write_line(
            2,
            "# YES"
        );

        lcd_write_line(
            3,
            "D NO"
        );
    }
    else if (config_screen ==
             CONFIG_LOG_VIEW)
    {
        size_t total_logs =
            event_log_count();

        if (!event_log_ready)
        {
            lcd_write_line(
                0,
                "EVENT LOG"
            );

            lcd_write_line(
                1,
                "Logging unavailable"
            );

            lcd_write_line(
                2,
                "Check NVS status"
            );
        }
        else if (total_logs == 0)
        {
            lcd_write_line(
                0,
                "EVENT LOG"
            );

            lcd_write_line(
                1,
                "No stored events"
            );

            lcd_write_line(
                2,
                ""
            );
        }
        else
        {
            if (log_selected_offset >=
                total_logs)
            {
                log_selected_offset =
                    total_logs - 1U;
            }

            event_log_record_t record;

            bool record_found =
                event_log_get_newest(
                    log_selected_offset,
                    &record
                );

            if (record_found)
            {
                uint8_t displayed_index =
                    (uint8_t)(
                        log_selected_offset +
                        1U
                    );

                uint8_t displayed_total =
                    (uint8_t)total_logs;

                snprintf(
                    line,
                    sizeof(line),
                    "LOG %02hhu/%02hhu",
                    (unsigned int)
                        displayed_index,
                    (unsigned int)
                        displayed_total
                );

                lcd_write_line(
                    0,
                    line
                );

                lcd_write_line(
                    1,
                    event_log_type_name(
                        record.type
                    )
                );

                format_event_timestamp(
                    &record,
                    line,
                    sizeof(line)
                );

                lcd_write_line(
                    2,
                    line
                );
            }
            else
            {
                lcd_write_line(
                    0,
                    "EVENT LOG ERROR"
                );

                lcd_write_line(
                    1,
                    "Record unavailable"
                );

                lcd_write_line(
                    2,
                    ""
                );
            }
        }

        lcd_write_line(
            3,
            "A:NEW B:OLD C:CLR"
        );
    }
    else if (config_screen ==
             CONFIG_LOG_CLEAR_CONFIRM)
    {
        uint8_t displayed_total =
            (uint8_t)
                event_log_count();

        lcd_write_line(
            0,
            "CLEAR ALL LOGS?"
        );

        snprintf(
            line,
            sizeof(line),
            "Stored events: %02hhu",
            (unsigned int)
                displayed_total
        );

        lcd_write_line(
            1,
            line
        );

        lcd_write_line(
            2,
            "# YES"
        );

        lcd_write_line(
            3,
            "D NO"
        );
    }
    else if (config_screen ==
             CONFIG_MESSAGE)
    {
        lcd_write_line(
            0,
            "CONFIGURATION"
        );

        lcd_write_line(
            1,
            config_message_line1
        );

        lcd_write_line(
            2,
            config_message_line2
        );

        lcd_write_line(
            3,
            "Please wait..."
        );
    }

    config_display_dirty = false;
}

/* =========================================================
 * Save time/date/duration
 * ========================================================= */

static void save_configured_time(
    TickType_t now
)
{
    if (config_input_length != 4)
    {
        show_config_message(
            "INVALID TIME",
            "Enter exactly HHMM",
            CONFIG_MENU,
            now
        );

        return;
    }

    uint8_t hour =
        (uint8_t)(
            (config_input[0] - '0') * 10 +
            (config_input[1] - '0')
        );

    uint8_t minute =
        (uint8_t)(
            (config_input[2] - '0') * 10 +
            (config_input[3] - '0')
        );

    if (hour > 23U ||
        minute > 59U)
    {
        show_config_message(
            "INVALID TIME",
            "Use 00:00-23:59",
            CONFIG_MENU,
            now
        );

        return;
    }

    esp_err_t result =
        rtc_write_time(
            hour,
            minute,
            0
        );

    if (result == ESP_OK)
    {
        reset_rtc_validation();

        show_config_message(
            "TIME SAVED",
            "Seconds reset to 00",
            CONFIG_MENU,
            now
        );
    }
    else
    {
        show_config_message(
            "RTC WRITE FAILED",
            esp_err_to_name(result),
            CONFIG_MENU,
            now
        );
    }
}

static void save_configured_date(
    TickType_t now
)
{
    if (config_input_length != 8)
    {
        show_config_message(
            "INVALID DATE",
            "Use DDMMYYYY",
            CONFIG_MENU,
            now
        );

        return;
    }

    uint8_t date =
        (uint8_t)(
            (config_input[0] - '0') * 10 +
            (config_input[1] - '0')
        );

    uint8_t month =
        (uint8_t)(
            (config_input[2] - '0') * 10 +
            (config_input[3] - '0')
        );

    uint16_t year =
        (uint16_t)(
            (config_input[4] - '0') * 1000 +
            (config_input[5] - '0') * 100 +
            (config_input[6] - '0') * 10 +
            (config_input[7] - '0')
        );

    if (year < RTC_MIN_YEAR ||
        year > RTC_MAX_YEAR ||
        month < 1U ||
        month > 12U ||
        date < 1U ||
        date >
            days_in_month(
                year,
                month
            ))
    {
        show_config_message(
            "INVALID DATE",
            "Check day/month/year",
            CONFIG_MENU,
            now
        );

        return;
    }

    esp_err_t result =
        rtc_write_date(
            year,
            month,
            date
        );

    if (result == ESP_OK)
    {
        reset_rtc_validation();

        show_config_message(
            "DATE SAVED",
            "Weekday calculated",
            CONFIG_MENU,
            now
        );
    }
    else
    {
        show_config_message(
            "RTC WRITE FAILED",
            esp_err_to_name(result),
            CONFIG_MENU,
            now
        );
    }
}

static void save_configured_duration(
    TickType_t now
)
{
    if (config_input_length == 0)
    {
        show_config_message(
            "INVALID DURATION",
            "Enter 1-60 seconds",
            CONFIG_MENU,
            now
        );

        return;
    }

    int seconds =
        input_to_integer();

    if (seconds <
            (int)MIN_RING_DURATION_SECONDS ||
        seconds >
            (int)MAX_RING_DURATION_SECONDS)
    {
        show_config_message(
            "INVALID DURATION",
            "Allowed: 1-60 sec",
            CONFIG_MENU,
            now
        );

        return;
    }

    ring_duration_ms =
        (uint32_t)seconds *
        1000U;

    esp_err_t result =
        save_ring_duration(
            ring_duration_ms
        );

    nvs_error =
        result != ESP_OK;

    if (result == ESP_OK)
    {
        char details[21];

        snprintf(
            details,
            sizeof(details),
            "%d seconds",
            seconds
        );

        show_config_message(
            "DURATION SAVED",
            details,
            CONFIG_MENU,
            now
        );
    }
    else
    {
        show_config_message(
            "DURATION UPDATED",
            "NVS save failed",
            CONFIG_MENU,
            now
        );
    }
}

/* =========================================================
 * Timetable keypad operations
 * ========================================================= */

static void save_new_timetable_bell(
    TickType_t now
)
{
    if (config_input_length != 5)
    {
        show_config_message(
            "INVALID BELL",
            "Use DHHMM",
            CONFIG_TIMETABLE_MENU,
            now
        );

        return;
    }

    uint8_t weekday =
        (uint8_t)(
            config_input[0] - '0'
        );

    uint8_t hour =
        (uint8_t)(
            (config_input[1] - '0') * 10 +
            (config_input[2] - '0')
        );

    uint8_t minute =
        (uint8_t)(
            (config_input[3] - '0') * 10 +
            (config_input[4] - '0')
        );

    bell_time_t new_entry = {
        .weekday = weekday,
        .hour = hour,
        .minute = minute,
        .second = 0
    };

    if (!timetable_entry_is_valid(
            &new_entry
        ))
    {
        show_config_message(
            "INVALID BELL",
            "Day 1-7 HH 00-23",
            CONFIG_TIMETABLE_MENU,
            now
        );

        return;
    }

    if (timetable_size >=
        MAX_TIMETABLE_ENTRIES)
    {
        show_config_message(
            "TIMETABLE FULL",
            "Maximum 80 bells",
            CONFIG_TIMETABLE_MENU,
            now
        );

        return;
    }

    if (timetable_contains_entry(
            &new_entry
        ))
    {
        show_config_message(
            "DUPLICATE BELL",
            "Time already exists",
            CONFIG_TIMETABLE_MENU,
            now
        );

        return;
    }

    esp_err_t result =
        add_timetable_bell(
            &new_entry
        );

    nvs_error =
        result != ESP_OK;

    if (result == ESP_OK)
    {
        char details[21];

        snprintf(
            details,
            sizeof(details),
            "%s %02u:%02u",
            weekday_name(weekday),
            hour,
            minute
        );

        show_config_message(
            "BELL ADDED",
            details,
            CONFIG_TIMETABLE_MENU,
            now
        );
    }
    else
    {
        show_config_message(
            "ADD FAILED",
            esp_err_to_name(result),
            CONFIG_TIMETABLE_MENU,
            now
        );
    }
}

static void delete_selected_timetable_bell(
    TickType_t now
)
{
    esp_err_t result =
        delete_timetable_bell(
            timetable_selected_index
        );

    nvs_error =
        result != ESP_OK;

    if (result == ESP_OK)
    {
        if (timetable_selected_index >=
            timetable_size)
        {
            timetable_selected_index =
                timetable_size - 1;
        }

        show_config_message(
            "BELL DELETED",
            "Timetable saved",
            CONFIG_TIMETABLE_DELETE,
            now
        );
    }
    else if (timetable_size <= 1)
    {
        show_config_message(
            "DELETE BLOCKED",
            "Keep at least 1 bell",
            CONFIG_TIMETABLE_DELETE,
            now
        );
    }
    else
    {
        show_config_message(
            "DELETE FAILED",
            esp_err_to_name(result),
            CONFIG_TIMETABLE_DELETE,
            now
        );
    }
}

static void restore_default_from_keypad(
    TickType_t now
)
{
    esp_err_t result =
        restore_default_timetable();

    nvs_error =
        result != ESP_OK;

    if (result == ESP_OK)
    {
        timetable_selected_index = 0;

        show_config_message(
            "DEFAULTS RESTORED",
            "75 bells loaded",
            CONFIG_TIMETABLE_MENU,
            now
        );
    }
    else
    {
        show_config_message(
            "RESTORE FAILED",
            esp_err_to_name(result),
            CONFIG_TIMETABLE_MENU,
            now
        );
    }
}

/* =========================================================
 * Keypad menu handling
 * ========================================================= */

static void handle_keypad_key(
    char key,
    TickType_t now
)
{
    if (key == '\0')
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Keypad key: %c",
        key
    );

    if (config_screen ==
        CONFIG_CLOSED)
    {
        if (key == 'A')
        {
            if (access_control_ready)
            {
                begin_admin_login(now);
            }
            else
            {
                show_config_message(
                    "ACCESS UNAVAILABLE",
                    "Check NVS status",
                    CONFIG_CLOSED,
                    now
                );
            }
        }

        return;
    }

    config_last_activity_tick =
        now;

    if (config_screen ==
        CONFIG_ADMIN_LOGIN)
    {
        if (key >= '0' &&
            key <= '9')
        {
            append_config_digit(key);
        }
        else if (key == '*')
        {
            delete_config_digit();
        }
        else if (key == 'D')
        {
            close_config_menu();
        }
        else if (key == '#')
        {
            uint32_t remaining_lockout_ms =
                0;

            uint32_t now_ms =
                (uint32_t)(
                    now *
                    portTICK_PERIOD_MS
                );

            access_result_t result =
                access_control_verify(
                    config_input,
                    now_ms,
                    &remaining_lockout_ms
                );

            if (result ==
                ACCESS_RESULT_GRANTED)
            {
                clear_config_input();

                open_config_menu(now);

                record_event(
                    EVENT_LOG_ADMIN_LOGIN_GRANTED,
                    0
                );

                ESP_LOGI(
                    TAG,
                    "Administrator configuration access granted"
                );
            }
            else if (result ==
                     ACCESS_RESULT_DENIED)
            {
                uint8_t failed =
                    access_control_failed_attempts();

                record_event(
                    EVENT_LOG_ADMIN_LOGIN_FAILED,
                    (int32_t)failed
                );

                uint8_t attempts_left =
                    failed >=
                        ACCESS_MAX_FAILED_ATTEMPTS
                        ? 0
                        : ACCESS_MAX_FAILED_ATTEMPTS -
                            failed;

                char details[21];

                snprintf(
                    details,
                    sizeof(details),
                    "Attempts left: %u",
                    attempts_left
                );

                clear_config_input();

                show_config_message(
                    "INCORRECT PIN",
                    details,
                    CONFIG_ADMIN_LOGIN,
                    now
                );
            }
            else if (result ==
                     ACCESS_RESULT_LOCKED)
            {
                clear_config_input();

                if (remaining_lockout_ms == 0U)
                {
                    remaining_lockout_ms =
                        ACCESS_LOCKOUT_MS;
                }

                admin_lockout_end_tick =
                    now +
                    pdMS_TO_TICKS(
                        remaining_lockout_ms
                    );

                admin_last_remaining_seconds =
                    UINT32_MAX;

                config_screen =
                    CONFIG_ADMIN_LOCKED;

                config_last_activity_tick =
                    now;

                config_display_dirty =
                    true;

                record_event(
                    EVENT_LOG_ADMIN_LOCKED,
                    (int32_t)(
                        ACCESS_LOCKOUT_MS /
                        1000U
                    )
                );

                ESP_LOGW(
                    TAG,
                    "Administrator configuration access locked"
                );
            }
            else
            {
                clear_config_input();

                show_config_message(
                    "ACCESS ERROR",
                    "PIN system failed",
                    CONFIG_CLOSED,
                    now
                );
            }
        }

        return;
    }

    if (config_screen ==
        CONFIG_ADMIN_LOCKED)
    {
        if (key == 'D')
        {
            close_config_menu();
        }

        return;
    }

    if (config_screen ==
        CONFIG_CHANGE_PIN_NEW)
    {
        if (key >= '0' &&
            key <= '9')
        {
            append_config_digit(key);
        }
        else if (key == '*')
        {
            delete_config_digit();
        }
        else if (key == 'D')
        {
            clear_config_input();

            memset(
                pending_admin_pin,
                0,
                sizeof(pending_admin_pin)
            );

            config_screen =
                CONFIG_MENU;

            config_display_dirty =
                true;
        }
        else if (key == '#')
        {
            if (!access_control_pin_is_valid(
                    config_input
                ))
            {
                clear_config_input();

                show_config_message(
                    "INVALID NEW PIN",
                    "Use 4-8 digits",
                    CONFIG_CHANGE_PIN_NEW,
                    now
                );

                return;
            }

            memcpy(
                pending_admin_pin,
                config_input,
                config_input_length
            );

            pending_admin_pin[
                config_input_length
            ] = '\0';

            clear_config_input();

            config_screen =
                CONFIG_CHANGE_PIN_CONFIRM;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_CHANGE_PIN_CONFIRM)
    {
        if (key >= '0' &&
            key <= '9')
        {
            append_config_digit(key);
        }
        else if (key == '*')
        {
            delete_config_digit();
        }
        else if (key == 'D')
        {
            clear_config_input();

            memset(
                pending_admin_pin,
                0,
                sizeof(pending_admin_pin)
            );

            config_screen =
                CONFIG_MENU;

            config_display_dirty =
                true;
        }
        else if (key == '#')
        {
            if (strcmp(
                    pending_admin_pin,
                    config_input
                ) != 0)
            {
                clear_config_input();

                memset(
                    pending_admin_pin,
                    0,
                    sizeof(pending_admin_pin)
                );

                show_config_message(
                    "PINS DO NOT MATCH",
                    "Enter new PIN again",
                    CONFIG_CHANGE_PIN_NEW,
                    now
                );

                return;
            }

            esp_err_t pin_result =
                access_control_change_pin(
                    pending_admin_pin
                );

            clear_config_input();

            memset(
                pending_admin_pin,
                0,
                sizeof(pending_admin_pin)
            );

            if (pin_result == ESP_OK)
            {
                /*
                 * A changed PIN invalidates every existing
                 * browser session.
                 */
                web_auth_logout_all();

                record_event(
                    EVENT_LOG_ADMIN_PIN_CHANGED,
                    0
                );

                ESP_LOGI(
                    TAG,
                    "Administrator PIN changed from keypad"
                );

                show_config_message(
                    "PIN CHANGED",
                    "Use the new PIN",
                    CONFIG_MENU,
                    now
                );
            }
            else
            {
                show_config_message(
                    "PIN SAVE FAILED",
                    esp_err_to_name(
                        pin_result
                    ),
                    CONFIG_MENU,
                    now
                );
            }
        }

        return;
    }

    if (config_screen ==
        CONFIG_MESSAGE)
    {
        if (key == 'D')
        {
            config_screen =
                config_message_return_screen;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_MENU)
    {
        if (key == '1')
        {
            clear_config_input();
            config_screen =
                CONFIG_SET_TIME;
        }
        else if (key == '2')
        {
            clear_config_input();
            config_screen =
                CONFIG_SET_DATE;
        }
        else if (key == '3')
        {
            clear_config_input();
            config_screen =
                CONFIG_SET_DURATION;
        }
        else if (key == '4')
        {
            close_config_menu();
            start_alarm();

            record_event(
                EVENT_LOG_BELL_TEST,
                (int32_t)ring_duration_ms
            );

            return;
        }
        else if (key == '5')
        {
            bool saved =
                apply_auto_state(
                    !auto_enabled
                );

            show_config_message(
                auto_enabled
                    ? "AUTO MODE ON"
                    : "AUTO MODE OFF",

                saved
                    ? "Setting saved"
                    : "Runtime only",

                CONFIG_MENU,
                now
            );

            return;
        }
        else if (key == '6')
        {
            config_screen =
                CONFIG_TIMETABLE_MENU;
        }
        else if (key == '7')
        {
            /*
             * Keep the administrator session active while
             * entering the event-log viewer.
             */
            config_last_activity_tick =
                now;

            if (!event_log_ready)
            {
                show_config_message(
                    "LOG UNAVAILABLE",
                    "Check NVS status",
                    CONFIG_MENU,
                    now
                );

                return;
            }

            log_selected_offset = 0;

            config_screen =
                CONFIG_LOG_VIEW;

            config_display_dirty =
                true;

            /*
             * Draw immediately so the normal alarm screen
             * cannot replace the log viewer.
             */
            display_config_screen();

            ESP_LOGI(
                TAG,
                "Event-log viewer opened"
            );

            return;
        }
        else if (key == '8')
        {
            clear_config_input();

            memset(
                pending_admin_pin,
                0,
                sizeof(pending_admin_pin)
            );

            config_screen =
                CONFIG_CHANGE_PIN_NEW;

            config_display_dirty =
                true;
        }
        else if (key == 'D')
        {
            close_config_menu();
            return;
        }

        config_display_dirty = true;
        return;
    }

    if (config_screen ==
        CONFIG_TIMETABLE_MENU)
    {
        if (key == '1')
        {
            timetable_selected_index = 0;

            config_screen =
                CONFIG_TIMETABLE_VIEW;
        }
        else if (key == '2')
        {
            clear_config_input();

            config_screen =
                CONFIG_TIMETABLE_ADD;
        }
        else if (key == '3')
        {
            timetable_selected_index = 0;

            config_screen =
                CONFIG_TIMETABLE_DELETE;
        }
        else if (key == '4')
        {
            config_screen =
                CONFIG_TIMETABLE_CONFIRM_DEFAULT;
        }
        else if (key == 'D' ||
                 key == 'A')
        {
            config_screen =
                CONFIG_MENU;
        }

        config_display_dirty = true;
        return;
    }

    if (config_screen ==
        CONFIG_TIMETABLE_VIEW)
    {
        if (key == 'A')
        {
            select_previous_timetable_entry();
        }
        else if (key == 'B')
        {
            select_next_timetable_entry();
        }
        else if (key == 'D')
        {
            config_screen =
                CONFIG_TIMETABLE_MENU;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_TIMETABLE_DELETE)
    {
        if (key == 'A')
        {
            select_previous_timetable_entry();
        }
        else if (key == 'B')
        {
            select_next_timetable_entry();
        }
        else if (key == '#')
        {
            delete_selected_timetable_bell(
                now
            );
        }
        else if (key == 'D')
        {
            config_screen =
                CONFIG_TIMETABLE_MENU;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_TIMETABLE_CONFIRM_DEFAULT)
    {
        if (key == '#')
        {
            restore_default_from_keypad(
                now
            );
        }
        else if (key == 'D')
        {
            config_screen =
                CONFIG_TIMETABLE_MENU;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_LOG_VIEW)
    {
        if (key == 'A')
        {
            select_newer_log_event();
        }
        else if (key == 'B')
        {
            select_older_log_event();
        }
        else if (key == 'C')
        {
            config_screen =
                CONFIG_LOG_CLEAR_CONFIRM;

            config_display_dirty =
                true;
        }
        else if (key == 'D')
        {
            config_screen =
                CONFIG_MENU;

            config_display_dirty =
                true;
        }

        return;
    }

    if (config_screen ==
        CONFIG_LOG_CLEAR_CONFIRM)
    {
        if (key == '#')
        {
            esp_err_t clear_result =
                event_log_clear();

            if (clear_result == ESP_OK)
            {
                log_selected_offset = 0;

                /*
                 * Keep one audit record showing that
                 * the previous log was cleared.
                 */
                record_event(
                    EVENT_LOG_LOGS_CLEARED,
                    0
                );

                show_config_message(
                    "LOGS CLEARED",
                    "Audit entry saved",
                    CONFIG_LOG_VIEW,
                    now
                );
            }
            else
            {
                show_config_message(
                    "CLEAR FAILED",
                    esp_err_to_name(
                        clear_result
                    ),
                    CONFIG_LOG_VIEW,
                    now
                );
            }
        }
        else if (key == 'D')
        {
            config_screen =
                CONFIG_LOG_VIEW;

            config_display_dirty =
                true;
        }

        return;
    }

    if (key >= '0' &&
        key <= '9')
    {
        append_config_digit(key);
        return;
    }

    if (key == '*')
    {
        delete_config_digit();
        return;
    }

    if (key == 'D' ||
        key == 'A')
    {
        clear_config_input();

        if (config_screen ==
            CONFIG_TIMETABLE_ADD)
        {
            config_screen =
                CONFIG_TIMETABLE_MENU;
        }
        else
        {
            config_screen =
                CONFIG_MENU;
        }

        config_display_dirty =
            true;

        return;
    }

    if (key == '#')
    {
        if (config_screen ==
            CONFIG_SET_TIME)
        {
            save_configured_time(now);
        }
        else if (config_screen ==
                 CONFIG_SET_DATE)
        {
            save_configured_date(now);
        }
        else if (config_screen ==
                 CONFIG_SET_DURATION)
        {
            save_configured_duration(now);
        }
        else if (config_screen ==
                 CONFIG_TIMETABLE_ADD)
        {
            save_new_timetable_bell(now);
        }
    }
}

/* =========================================================
 * Main application
 * ========================================================= */


/* =========================================================
 * Safe web timetable command processing
 * ========================================================= */

/*
 * Only the main alarm task calls this function.
 *
 * The HTTP server sends requests through a FreeRTOS queue.
 * This prevents the HTTP task from directly modifying the
 * timetable array while the alarm task is using it.
 */
static void process_web_timetable_requests(void)
{
    web_timetable_request_t request;

    while (
        web_timetable_bridge_receive(
            &request
        )
    )
    {
        esp_err_t result =
            ESP_ERR_INVALID_ARG;

        switch (request.type)
        {
            case WEB_TIMETABLE_COMMAND_GET_ALL:
            {
                if (
                    request.output_entries == NULL ||
                    request.output_count == NULL
                )
                {
                    result =
                        ESP_ERR_INVALID_ARG;

                    break;
                }

                if (
                    request.output_capacity <
                        (size_t)timetable_size
                )
                {
                    result =
                        ESP_ERR_INVALID_SIZE;

                    break;
                }

                for (int i = 0;
                     i < timetable_size;
                     i++)
                {
                    request.output_entries[i] =
                        (web_timetable_entry_t){
                            .weekday =
                                timetable[i].weekday,

                            .hour =
                                timetable[i].hour,

                            .minute =
                                timetable[i].minute,

                            .second =
                                timetable[i].second
                        };
                }

                *request.output_count =
                    (size_t)timetable_size;

                result = ESP_OK;

                break;
            }

            case WEB_TIMETABLE_COMMAND_ADD:
            {
                bell_time_t new_entry = {
                    .weekday =
                        request.entry.weekday,

                    .hour =
                        request.entry.hour,

                    .minute =
                        request.entry.minute,

                    .second =
                        request.entry.second
                };

                result =
                    add_timetable_bell(
                        &new_entry
                    );

                break;
            }

            case WEB_TIMETABLE_COMMAND_DELETE:
            {
                if (
                    request.index >=
                        (size_t)timetable_size
                )
                {
                    result =
                        ESP_ERR_INVALID_ARG;

                    break;
                }

                result =
                    delete_timetable_bell(
                        (int)request.index
                    );

                break;
            }

            case WEB_TIMETABLE_COMMAND_RESTORE_DEFAULTS:
            {
                result =
                    restore_default_timetable();

                break;
            }

            default:
            {
                result =
                    ESP_ERR_INVALID_ARG;

                break;
            }
        }

        /*
         * GET_ALL does not modify persistent state.
         */
        if (
            request.type !=
                WEB_TIMETABLE_COMMAND_GET_ALL
        )
        {
            nvs_error =
                result != ESP_OK;

            if (result == ESP_OK)
            {
                /*
                 * Keep the keypad timetable viewer valid
                 * after a web add, delete or restore.
                 */
                if (
                    timetable_selected_index >=
                        timetable_size
                )
                {
                    timetable_selected_index =
                        timetable_size - 1;
                }

                if (timetable_selected_index < 0)
                {
                    timetable_selected_index = 0;
                }

                config_display_dirty = true;
                last_display_second = -1;

                ESP_LOGI(
                    TAG,
                    "Web timetable command completed"
                );
            }
            else
            {
                ESP_LOGE(
                    TAG,
                    "Web timetable command failed: %s",
                    esp_err_to_name(result)
                );
            }
        }

        web_timetable_bridge_complete(
            &request,
            result
        );
    }
}


/* =========================================================
 * Safe web event-log command processing
 * ========================================================= */

/*
 * Only the main alarm task accesses the event-log storage
 * for web requests.
 */
static void process_web_event_requests(void)
{
    web_event_request_t request;

    while (
        web_event_bridge_receive(
            &request
        )
    )
    {
        esp_err_t result =
            ESP_ERR_INVALID_ARG;

        switch (request.type)
        {
            case WEB_EVENT_COMMAND_GET_ALL:
            {
                if (!event_log_ready)
                {
                    result =
                        ESP_ERR_INVALID_STATE;

                    break;
                }

                if (
                    request.output_records == NULL ||
                    request.output_count == NULL
                )
                {
                    result =
                        ESP_ERR_INVALID_ARG;

                    break;
                }

                size_t total =
                    event_log_count();

                if (
                    total >
                        request.output_capacity
                )
                {
                    result =
                        ESP_ERR_INVALID_SIZE;

                    break;
                }

                bool read_failed =
                    false;

                for (size_t i = 0U;
                     i < total;
                     i++)
                {
                    if (!event_log_get_newest(
                            i,
                            &request.output_records[i]
                        ))
                    {
                        read_failed = true;
                        break;
                    }
                }

                if (read_failed)
                {
                    result = ESP_FAIL;
                    break;
                }

                *request.output_count =
                    total;

                result = ESP_OK;

                break;
            }

            case WEB_EVENT_COMMAND_CLEAR:
            {
                if (!event_log_ready)
                {
                    result =
                        ESP_ERR_INVALID_STATE;

                    break;
                }

                result =
                    event_log_clear();

                if (result == ESP_OK)
                {
                    log_selected_offset = 0U;

                    /*
                     * Keep one audit entry proving that the
                     * older records were cleared.
                     */
                    record_event(
                        EVENT_LOG_LOGS_CLEARED,
                        0
                    );

                    config_display_dirty = true;
                    last_display_second = -1;

                    ESP_LOGI(
                        TAG,
                        "Event log cleared from web"
                    );
                }

                break;
            }

            default:
            {
                result =
                    ESP_ERR_INVALID_ARG;

                break;
            }
        }

        web_event_bridge_complete(
            &request,
            result
        );
    }
}


/* =========================================================
 * Announcement priority enforcement
 * ========================================================= */

/*
 * The announcement manager may be changed by another task,
 * such as the authenticated web server.
 *
 * The main alarm task checks the state continuously. If an
 * announcement begins while the bell is already ringing,
 * the bell is stopped immediately.
 */

static announcement_state_t
    previous_announcement_state =
        ANNOUNCEMENT_STATE_IDLE;

static bool announcement_event_tracking_ready =
    false;

/*
 * Store every announcement-state transition in the existing
 * circular NVS event log.
 *
 * A LIVE -> EMERGENCY transition creates:
 *   LIVE ANN STOPPED
 *   EMERGENCY STARTED
 *
 * An EMERGENCY -> LIVE transition creates:
 *   EMERGENCY STOPPED
 *   LIVE ANN STARTED
 */
static void track_announcement_events(void)
{
    announcement_status_t status;

    esp_err_t result =
        announcement_get_status(
            &status
        );

    if (result != ESP_OK)
    {
        return;
    }

    if (!announcement_event_tracking_ready)
    {
        previous_announcement_state =
            ANNOUNCEMENT_STATE_IDLE;

        announcement_event_tracking_ready =
            true;
    }

    if (
        status.state ==
        previous_announcement_state
    )
    {
        return;
    }

    /*
     * First record modes that ended.
     */
    if (
        previous_announcement_state ==
            ANNOUNCEMENT_STATE_LIVE &&
        status.state !=
            ANNOUNCEMENT_STATE_LIVE
    )
    {
        record_event(
            EVENT_LOG_ANNOUNCEMENT_LIVE_STOPPED,
            0
        );
    }

    if (
        previous_announcement_state ==
            ANNOUNCEMENT_STATE_EMERGENCY &&
        status.state !=
            ANNOUNCEMENT_STATE_EMERGENCY
    )
    {
        record_event(
            EVENT_LOG_ANNOUNCEMENT_EMERGENCY_STOPPED,
            0
        );
    }

    /*
     * Then record modes that started.
     */
    if (
        status.state ==
            ANNOUNCEMENT_STATE_EMERGENCY &&
        previous_announcement_state !=
            ANNOUNCEMENT_STATE_EMERGENCY
    )
    {
        record_event(
            EVENT_LOG_ANNOUNCEMENT_EMERGENCY_STARTED,
            1
        );
    }

    if (
        status.state ==
            ANNOUNCEMENT_STATE_LIVE &&
        previous_announcement_state !=
            ANNOUNCEMENT_STATE_LIVE
    )
    {
        record_event(
            EVENT_LOG_ANNOUNCEMENT_LIVE_STARTED,
            1
        );
    }

    ESP_LOGI(
        TAG,
        "Announcement audit: %s -> %s",
        announcement_state_name(
            previous_announcement_state
        ),
        announcement_state_name(
            status.state
        )
    );

    previous_announcement_state =
        status.state;

    /*
     * Force the LCD to immediately display the new state.
     */
    last_display_second = -1;
}

static void enforce_announcement_priority(void)
{
    if (!announcement_blocks_bell())
    {
        return;
    }

    if (!alarm_active)
    {
        return;
    }

    stop_alarm();

    last_display_second = -1;

    ESP_LOGW(
        TAG,
        "Bell stopped because announcement has priority"
    );
}

static esp_err_t set_auto_from_web(
    bool enabled
)
{
    if (!nvs_available)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (auto_enabled == enabled)
    {
        return ESP_OK;
    }

    bool saved =
        apply_auto_state(
            enabled
        );

    return saved
        ? ESP_OK
        : ESP_FAIL;
}

static esp_err_t set_duration_from_web(
    uint32_t seconds
)
{
    if (
        seconds <
            MIN_RING_DURATION_SECONDS ||
        seconds >
            MAX_RING_DURATION_SECONDS
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!nvs_available)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t new_duration_ms =
        seconds * 1000U;

    if (ring_duration_ms ==
        new_duration_ms)
    {
        return ESP_OK;
    }

    esp_err_t result =
        save_ring_duration(
            new_duration_ms
        );

    nvs_error =
        result != ESP_OK;

    if (result != ESP_OK)
    {
        return result;
    }

    ring_duration_ms =
        new_duration_ms;

    record_event(
        EVENT_LOG_DURATION_CHANGED,
        (int32_t)seconds
    );

    ESP_LOGI(
        TAG,
        "Ring duration changed from web to %lu seconds",
        (unsigned long)seconds
    );

    return ESP_OK;
}

static const web_control_handlers_t
    web_control_handlers = {
        .set_auto_enabled =
            set_auto_from_web,

        .set_ring_duration =
            set_duration_from_web
    };

static void provide_web_status(
    web_status_t *status
)
{
    if (status == NULL)
    {
        return;
    }

    /*
     * Copy the latest RTC data into a local snapshot.
     * The web server only reads controller state.
     */
    rtc_time_t rtc_snapshot =
        latest_rtc_time;

    bool rtc_snapshot_valid =
        latest_rtc_time_valid &&
        rtc_ready;

    status->auto_enabled =
        auto_enabled;

    status->alarm_active =
        alarm_active;

    status->rtc_ready =
        rtc_ready;

    status->wifi_connected =
        wifi_manager_is_connected();

    status->timetable_count =
        timetable_size > 0
            ? (uint32_t)timetable_size
            : 0U;

    status->event_log_count =
        (uint32_t)event_log_count();

    status->mqtt_connected =
        mqtt_manager_is_connected();

    /*
     * Read-only announcement status for the public
     * dashboard and /api/status endpoint.
     */
    announcement_status_t announcement_snapshot;

    memset(
        &announcement_snapshot,
        0,
        sizeof(announcement_snapshot)
    );

    esp_err_t announcement_status_result =
        announcement_get_status(
            &announcement_snapshot
        );

    if (announcement_status_result == ESP_OK)
    {
        snprintf(
            status->announcement_state,
            sizeof(status->announcement_state),
            "%s",
            announcement_state_name(
                announcement_snapshot.state
            )
        );

        status->announcement_pa_active =
            announcement_output_is_active();

        status->bell_blocked_by_announcement =
            announcement_blocks_bell();
    }
    else
    {
        snprintf(
            status->announcement_state,
            sizeof(status->announcement_state),
            "UNAVAILABLE"
        );

        status->announcement_pa_active =
            false;

        status->bell_blocked_by_announcement =
            false;
    }

    mqtt_diagnostics_t mqtt_diagnostics;

    memset(
        &mqtt_diagnostics,
        0,
        sizeof(mqtt_diagnostics)
    );

    mqtt_manager_get_diagnostics(
        &mqtt_diagnostics
    );

    status->mqtt_disconnect_count =
        mqtt_diagnostics.disconnect_count;

    snprintf(
        status->mqtt_broker,
        sizeof(status->mqtt_broker),
        "%s",
        mqtt_manager_broker_uri()
    );

    snprintf(
        status->mqtt_diagnostic,
        sizeof(status->mqtt_diagnostic),
        "%s",
        mqtt_diagnostics.summary
    );

    const char *mqtt_topic =
        mqtt_manager_topic_prefix();

    snprintf(
        status->mqtt_topic,
        sizeof(status->mqtt_topic),
        "%s",
        (
            mqtt_topic != NULL &&
            mqtt_topic[0] != '\0'
        )
            ? mqtt_topic
            : "Unavailable"
    );

    /*
     * Storage health and capacity.
     */
    status->nvs_ready =
        nvs_available &&
        !nvs_error;

    nvs_stats_t nvs_statistics = {0};

    esp_err_t nvs_statistics_result =
        nvs_get_stats(
            "nvs",
            &nvs_statistics
        );

    if (nvs_statistics_result == ESP_OK)
    {
        status->nvs_total_entries =
            (uint32_t)
                nvs_statistics.total_entries;

        status->nvs_used_entries =
            (uint32_t)
                nvs_statistics.used_entries;

        status->nvs_free_entries =
            (uint32_t)
                nvs_statistics.free_entries;
    }

    const esp_partition_t *running_partition =
        esp_ota_get_running_partition();

    if (running_partition != NULL)
    {
        status->app_partition_size_bytes =
            (uint32_t)
                running_partition->size;
    }

    status->timetable_capacity =
        MAX_TIMETABLE_ENTRIES;

    status->event_log_capacity =
        EVENT_LOG_MAX_RECORDS;

    status->ring_duration_seconds =
        (
            ring_duration_ms +
            999U
        ) /
        1000U;

    status->uptime_seconds =
        (uint32_t)(
            pdTICKS_TO_MS(
                xTaskGetTickCount()
            ) /
            1000U
        );

    snprintf(
        status->wifi_state,
        sizeof(status->wifi_state),
        "%s",
        wifi_manager_state_name(
            wifi_manager_get_state()
        )
    );

    if (
        wifi_manager_get_ip_string(
            status->ip_address,
            sizeof(status->ip_address)
        ) != ESP_OK
    )
    {
        snprintf(
            status->ip_address,
            sizeof(status->ip_address),
            "0.0.0.0"
        );
    }

    if (!rtc_snapshot_valid)
    {
        snprintf(
            status->current_time,
            sizeof(status->current_time),
            "RTC unavailable"
        );

        snprintf(
            status->next_bell,
            sizeof(status->next_bell),
            "RTC unavailable"
        );

        return;
    }

    snprintf(
        status->current_time,
        sizeof(status->current_time),
        "%04u-%02u-%02u %02u:%02u:%02u",

        (unsigned int)
            rtc_snapshot.year,

        (unsigned int)
            rtc_snapshot.month,

        (unsigned int)
            rtc_snapshot.date,

        (unsigned int)
            rtc_snapshot.hour,

        (unsigned int)
            rtc_snapshot.minute,

        (unsigned int)
            rtc_snapshot.second
    );

    if (timetable_size <= 0)
    {
        snprintf(
            status->next_bell,
            sizeof(status->next_bell),
            "No bells configured"
        );

        return;
    }

    int current_day_seconds =
        (
            (int)rtc_snapshot.hour *
            3600
        ) +
        (
            (int)rtc_snapshot.minute *
            60
        ) +
        (int)rtc_snapshot.second;

    int best_index = -1;

    int64_t best_delta_seconds =
        7LL *
        24LL *
        60LL *
        60LL +
        1LL;

    for (int i = 0;
         i < timetable_size;
         i++)
    {
        int day_difference =
            (int)timetable[i].weekday -
            (int)rtc_snapshot.weekday;

        while (day_difference < 0)
        {
            day_difference += 7;
        }

        while (day_difference >= 7)
        {
            day_difference -= 7;
        }

        int bell_day_seconds =
            (
                (int)timetable[i].hour *
                3600
            ) +
            (
                (int)timetable[i].minute *
                60
            ) +
            (int)timetable[i].second;

        int64_t delta_seconds =
            (
                (int64_t)day_difference *
                86400LL
            ) +
            (
                (int64_t)bell_day_seconds -
                (int64_t)current_day_seconds
            );

        /*
         * A passed bell on the same weekday belongs to
         * the following week.
         */
        if (delta_seconds < 0)
        {
            delta_seconds +=
                7LL * 86400LL;
        }

        if (delta_seconds <
            best_delta_seconds)
        {
            best_delta_seconds =
                delta_seconds;

            best_index = i;
        }
    }

    if (best_index < 0)
    {
        snprintf(
            status->next_bell,
            sizeof(status->next_bell),
            "No upcoming bell"
        );

        return;
    }

    snprintf(
        status->next_bell,
        sizeof(status->next_bell),
        "Day %u %02u:%02u:%02u",

        (unsigned int)
            timetable[best_index].weekday,

        (unsigned int)
            timetable[best_index].hour,

        (unsigned int)
            timetable[best_index].minute,

        (unsigned int)
            timetable[best_index].second
    );
}


/* =========================================================
 * MQTT status publishing
 * ========================================================= */

#define MQTT_STATUS_INTERVAL_MS 10000U

static void publish_mqtt_status_if_due(void)
{
    static TickType_t last_attempt_tick = 0;
    static bool published_on_current_connection = false;

    /*
     * Publish immediately after every new MQTT connection.
     */
    if (!mqtt_manager_is_connected())
    {
        published_on_current_connection = false;
        return;
    }

    TickType_t now =
        xTaskGetTickCount();

    if (
        published_on_current_connection &&
        (
            now -
            last_attempt_tick
        ) <
            pdMS_TO_TICKS(
                MQTT_STATUS_INTERVAL_MS
            )
    )
    {
        return;
    }

    last_attempt_tick = now;

    static web_status_t web_status;

    memset(
        &web_status,
        0,
        sizeof(web_status)
    );

    provide_web_status(
        &web_status
    );

    static mqtt_alarm_status_t mqtt_status;

    memset(
        &mqtt_status,
        0,
        sizeof(mqtt_status)
    );

    mqtt_status = (mqtt_alarm_status_t){
        .auto_enabled =
            web_status.auto_enabled,

        .alarm_active =
            web_status.alarm_active,

        .rtc_ready =
            web_status.rtc_ready,

        .timetable_count =
            web_status.timetable_count,

        .event_log_count =
            web_status.event_log_count,

        .ring_duration_seconds =
            web_status.ring_duration_seconds,

        .uptime_seconds =
            web_status.uptime_seconds
    };

    snprintf(
        mqtt_status.current_time,
        sizeof(mqtt_status.current_time),
        "%s",
        web_status.current_time
    );

    snprintf(
        mqtt_status.next_bell,
        sizeof(mqtt_status.next_bell),
        "%s",
        web_status.next_bell
    );

    esp_err_t result =
        mqtt_manager_publish_status(
            &mqtt_status
        );

    if (result == ESP_OK)
    {
        published_on_current_connection = true;
    }
    else
    {
        ESP_LOGW(
            TAG,
            "MQTT status publish failed: %s",
            esp_err_to_name(result)
        );
    }
}

void app_main(void)
{
    /*
     * Announcement management is optional to local alarm
     * operation. If initialization fails, the normal RTC
     * alarm remains available.
     */
    esp_err_t announcement_result =
        announcement_manager_init();

    if (announcement_result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Announcement manager is ready"
        );

        esp_err_t controls_result =
            announcement_controls_init();

        if (controls_result == ESP_OK)
        {
            ESP_LOGI(
                TAG,
                "Announcement controls are ready"
            );
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Announcement controls failed: %s",
                esp_err_to_name(controls_result)
            );
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Announcement manager initialization failed: %s",
            esp_err_to_name(
                announcement_result
            )
        );
    }


    /*
     * Initialize communication between the HTTP server task
     * and the main alarm task.
     */
    ESP_ERROR_CHECK(
        web_timetable_bridge_init()
    );

    ESP_ERROR_CHECK(
        web_event_bridge_init()
    );

    /*
     * Capture this before initialization changes the system
     * state or produces other events.
     */
    startup_reset_reason =
        esp_reset_reason();

    ESP_LOGI(
        TAG,
        "Startup reset reason code: %d",
        (int)startup_reset_reason
    );

    configure_output_pin(
        LED_PIN
    );

    configure_output_pin(
        RELAY_PIN
    );

    configure_button_pin(
        MANUAL_BUTTON_PIN
    );

    configure_button_pin(
        AUTO_BUTTON_PIN
    );

    configure_keypad();

    /*
     * Initialize the schedule guard before RTC processing.
     */
    schedule_guard_init(
        &schedule_guard
    );

    /* =====================================================
     * Initialize NVS and stored settings
     * ===================================================== */

    esp_err_t nvs_result =
        initialize_nvs();

    if (nvs_result == ESP_OK)
    {
        nvs_available = true;
        nvs_error = false;

        esp_err_t auto_result =
            load_auto_state(
                &auto_enabled
            );

        if (auto_result != ESP_OK)
        {
            auto_enabled = false;
            nvs_error = true;
        }

        esp_err_t duration_result =
            load_ring_duration(
                &ring_duration_ms
            );

        if (duration_result != ESP_OK)
        {
            ring_duration_ms =
                DEFAULT_RING_DURATION_MS;

            nvs_error = true;
        }

#if !TEST_MODE
        initialize_timetable_storage();
#endif
    }
    else
    {
        nvs_available = false;
        nvs_error = true;

        auto_enabled = false;

        ring_duration_ms =
            DEFAULT_RING_DURATION_MS;

        load_default_timetable();

        ESP_LOGE(
            TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(
                nvs_result
            )
        );
    }


    /* =====================================================
     * Initialize persistent event logging
     * ===================================================== */

    if (nvs_available)
    {
        esp_err_t event_log_result =
            event_log_init();

        if (event_log_result == ESP_OK)
        {
            event_log_ready = true;

            ESP_LOGI(
                TAG,
                "Persistent event logging is ready"
            );
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Event log initialization failed: %s",
                esp_err_to_name(event_log_result)
            );
        }
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Event logging unavailable because NVS failed"
        );
    }

    /* =====================================================
     * Initialize administrator access control
     * ===================================================== */

    if (nvs_available)
    {
        esp_err_t access_result =
            access_control_init();

        if (access_result == ESP_OK)
        {
            access_control_ready = true;

            ESP_LOGI(
                TAG,
                "Administrator PIN protection is ready"
            );
        }
        else
        {
            access_control_ready = false;

            ESP_LOGE(
                TAG,
                "Administrator PIN protection failed: %s",
                esp_err_to_name(
                    access_result
                )
            );
        }
    }
    else
    {
        access_control_ready = false;

        ESP_LOGW(
            TAG,
            "Administrator PIN unavailable because NVS failed"
        );
    }

    /* =====================================================
     * Initialize hardware
     * ===================================================== */

    esp_err_t announcement_output_result =
        announcement_output_init();

    if (announcement_output_result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "PA announcement output is ready"
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "PA announcement output failed: %s",
            esp_err_to_name(
                announcement_output_result
            )
        );
    }

    configure_i2c();
    lcd_initialize();
    configure_buzzer();

    set_alarm_output(false);

    bool previous_manual_pressed =
        false;

    bool previous_auto_pressed =
        false;

    TickType_t last_manual_event =
        0;

    TickType_t last_auto_event =
        0;

    /* =====================================================
     * Initialize non-blocking Wi-Fi
     * ===================================================== */

    esp_err_t wifi_result =
        wifi_manager_init(
            SMART_ALARM_WIFI_SSID,
            SMART_ALARM_WIFI_PASSWORD
        );

    if (wifi_result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Wi-Fi connection process started"
        );
    }
    else
    {
        /*
         * Wi-Fi is optional for bell operation.
         * The offline alarm continues normally.
         */
        ESP_LOGW(
            TAG,
            "Wi-Fi initialization failed: %s",
            esp_err_to_name(
                wifi_result
            )
        );

        ESP_LOGW(
            TAG,
            "Continuing in offline alarm mode"
        );
    }

    /* =====================================================
     * Initialize the main task watchdog
     * ===================================================== */

    esp_err_t watchdog_result =
        system_watchdog_init(
            SYSTEM_WATCHDOG_TIMEOUT_MS
        );

    if (watchdog_result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Main task watchdog protection is ready"
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Main task watchdog initialization failed: %s",
            esp_err_to_name(
                watchdog_result
            )
        );
    }


    
    /*
     * Start MQTT after Wi-Fi and network initialization.
     *
     * Starting MQTT does not require an active connection.
     * The MQTT client reconnects automatically when the
     * network becomes available.
     */
    esp_err_t mqtt_start_result =
        mqtt_manager_start();

    if (mqtt_start_result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "MQTT status reporting started"
        );
    }
    else
    {
        /*
         * MQTT is optional. Local RTC alarm operation must
         * continue even when MQTT initialization fails.
         */
        ESP_LOGE(
            TAG,
            "MQTT start failed: %s",
            esp_err_to_name(
                mqtt_start_result
            )
        );
    }

while (1)
    {
        /*
         * Enforce live and emergency announcement priority
         * before processing normal bell operation.
         */
        /*
         * Read director push-to-talk and emergency controls
         * before processing normal bell operation.
         */
        (void)announcement_controls_poll();

        /*
         * Audit any LIVE or EMERGENCY state change.
         */
        track_announcement_events();

        /*
         * Enable or disable the simulated PA amplifier.
         */
        (void)announcement_output_sync();

        enforce_announcement_priority();


        /*
         * Publish one status snapshot every five seconds.
         * This function does nothing while MQTT is offline.
         */
        /*
         * Publish a retained status snapshot every
         * ten seconds while MQTT is connected.
         */
        publish_mqtt_status_if_due();


        /*
         * Process queued web timetable operations before
         * RTC, keypad or alarm checks. This also ensures the
         * queue is serviced during RTC faults or menu use.
         */
        process_web_timetable_requests();

        process_web_event_requests();

        TickType_t now =
            xTaskGetTickCount();

        /* =================================================
         * Keypad processing
         * ================================================= */

        char keypad_key =
            keypad_get_key_event(now);

        handle_keypad_key(
            keypad_key,
            now
        );

        if (
            config_screen ==
                CONFIG_MESSAGE &&
            (int32_t)(
                now -
                config_message_end_tick
            ) >= 0
        )
        {
            clear_config_input();

            config_screen =
                config_message_return_screen;

            config_last_activity_tick =
                now;

            config_display_dirty =
                true;
        }

        /*
         * Keep the administrator lockout screen updated.
         */
        if (config_screen ==
            CONFIG_ADMIN_LOCKED)
        {
            int32_t remaining_ticks =
                (int32_t)(
                    admin_lockout_end_tick -
                    now
                );

            if (remaining_ticks <= 0)
            {
                clear_config_input();

                config_screen =
                    CONFIG_ADMIN_LOGIN;

                config_last_activity_tick =
                    now;

                config_display_dirty =
                    true;

                admin_last_remaining_seconds =
                    UINT32_MAX;
            }
            else
            {
                uint32_t remaining_ms =
                    (uint32_t)
                        remaining_ticks *
                    portTICK_PERIOD_MS;

                uint32_t remaining_seconds =
                    (
                        remaining_ms +
                        999U
                    ) /
                    1000U;

                if (remaining_seconds !=
                    admin_last_remaining_seconds)
                {
                    admin_last_remaining_seconds =
                        remaining_seconds;

                    config_display_dirty =
                        true;
                }
            }
        }

        if (
            config_screen !=
                CONFIG_CLOSED &&
            config_screen !=
                CONFIG_ADMIN_LOCKED &&
            now -
                config_last_activity_tick >=
                pdMS_TO_TICKS(
                    CONFIG_TIMEOUT_MS
                )
        )
        {
            close_config_menu();
        }

        /*
         * All automatic alarms are paused while
         * configuration is open.
         */
        if (config_screen !=
            CONFIG_CLOSED)
        {
            stop_alarm();

            if (config_display_dirty)
            {
                display_config_screen();
            }

            /*
             * The configuration menu can remain open for
             * up to 30 seconds. Feed the 8-second watchdog
             * before continuing the loop.
             */
            if (system_watchdog_is_ready())
            {
                esp_err_t feed_result =
                    system_watchdog_feed();

                if (feed_result == ESP_OK)
                {
                    watchdog_feed_error_reported =
                        false;
                }
                else if (
                    !watchdog_feed_error_reported
                )
                {
                    watchdog_feed_error_reported =
                        true;

                    ESP_LOGE(
                        TAG,
                        "Failed to feed watchdog in menu: %s",
                        esp_err_to_name(
                            feed_result
                        )
                    );
                }
            }

            vTaskDelay(
                pdMS_TO_TICKS(50)
            );

            continue;
        }

        /* =================================================
         * RTC processing
         * ================================================= */

        rtc_time_t current_time;

        rtc_status_t rtc_status =
            rtc_read_time(
                &current_time
            );

        if (rtc_status !=
            RTC_STATUS_OK)
        {
            stop_alarm();

            if (previous_rtc_status !=
                rtc_status)
            {
                record_event(
                    EVENT_LOG_RTC_ERROR,
                    (int32_t)rtc_status
                );

                previous_rtc_status =
                    rtc_status;
            }

            rtc_ready = false;
            rtc_stable_readings = 0;

            schedule_guard_init(
                &schedule_guard
            );

            previous_schedule_guard_status =
                SCHEDULE_GUARD_FIRST_READING;

            schedule_guard_blocked_second =
                -1;

            display_rtc_fault(
                rtc_status
            );

            vTaskDelay(
                pdMS_TO_TICKS(200)
            );

            continue;
        }

        latest_rtc_time =
            current_time;

        latest_rtc_time_valid =
            true;

        if (!rtc_ready)
        {
            if (rtc_stable_readings <
                RTC_STABLE_READINGS_REQUIRED)
            {
                rtc_stable_readings++;
            }

            if (rtc_stable_readings <
                RTC_STABLE_READINGS_REQUIRED)
            {
                stop_alarm();

                display_rtc_recovering();

                vTaskDelay(
                    pdMS_TO_TICKS(200)
                );

                continue;
            }

            rtc_ready = true;

            if (previous_rtc_status !=
                RTC_STATUS_OK)
            {
                record_event(
                    EVENT_LOG_RTC_RECOVERED,
                    0
                );

                previous_rtc_status =
                    RTC_STATUS_OK;
            }

            ESP_LOGI(
                TAG,
                "RTC readings are stable"
            );

            last_display_second = -1;
        }

        /*
         * Record startup once the RTC is valid and stable.
         */
        if (rtc_ready &&
            !system_start_logged)
        {
            /*
             * Store the reset cause first so the history
             * explains why this startup happened.
             */
            record_event(
                reset_reason_to_event(
                    startup_reset_reason
                ),
                (int32_t)startup_reset_reason
            );

            record_event(
                EVENT_LOG_SYSTEM_START,
                0
            );

            system_start_logged = true;
        }

        /*
         * Compare this RTC reading with the previous
         * reading and classify the time movement.
         */
        schedule_clock_t current_schedule_clock =
            rtc_to_schedule_clock(
                &current_time
            );

        schedule_guard_status_t guard_status =
            schedule_guard_update(
                &schedule_guard,
                &current_schedule_clock,
                SCHEDULE_GRACE_SECONDS
            );

        /*
         * Large forward jumps and backward jumps block
         * scheduled ringing for the current RTC second.
         */
        if (guard_status ==
                SCHEDULE_GUARD_FORWARD_JUMP ||
            guard_status ==
                SCHEDULE_GUARD_BACKWARD_JUMP)
        {
            schedule_guard_blocked_second =
                schedule_guard.current_seconds;

            if (guard_status !=
                previous_schedule_guard_status)
            {
                record_event(
                    guard_status ==
                        SCHEDULE_GUARD_FORWARD_JUMP
                        ? EVENT_LOG_TIME_FORWARD_JUMP
                        : EVENT_LOG_TIME_BACKWARD_JUMP,

                    schedule_guard.elapsed_seconds
                );

                ESP_LOGW(
                    TAG,
                    "%s detected: %ld seconds",
                    guard_status ==
                        SCHEDULE_GUARD_FORWARD_JUMP
                        ? "Forward RTC jump"
                        : "Backward RTC jump",

                    (long)
                        schedule_guard.elapsed_seconds
                );
            }
        }

        previous_schedule_guard_status =
            guard_status;

        if (!timetable_initialized)
        {
            initialize_runtime_timetable(
                &current_time
            );

            last_display_second = -1;
        }

        /* =================================================
         * Scheduled alarm
         * ================================================= */

        bool already_triggered =
            last_trigger_year ==
                current_time.year &&

            last_trigger_month ==
                current_time.month &&

            last_trigger_date ==
                current_time.date &&

            last_trigger_hour ==
                current_time.hour &&

            last_trigger_minute ==
                current_time.minute &&

            last_trigger_second ==
                current_time.second;

        /*
         * Scheduled ringing is allowed only when time
         * moved normally or experienced a small delay.
         */
        bool schedule_time_is_safe =
            (
                guard_status ==
                    SCHEDULE_GUARD_NORMAL ||
                guard_status ==
                    SCHEDULE_GUARD_SMALL_DELAY
            ) &&
            schedule_guard.current_seconds !=
                schedule_guard_blocked_second;

        bool scheduled_bell_due =
            false;

        if (schedule_time_is_safe)
        {
            scheduled_bell_due =
                timetable_matches(
                    &current_time
                ) ||
                timetable_crossed_during_guard(
                    &current_time
                );
        }

        if (
            rtc_ready &&
            auto_enabled &&
            scheduled_bell_due &&
            !already_triggered
        )
        {
            last_trigger_year =
                current_time.year;

            last_trigger_month =
                current_time.month;

            last_trigger_date =
                current_time.date;

            last_trigger_hour =
                current_time.hour;

            last_trigger_minute =
                current_time.minute;

            last_trigger_second =
                current_time.second;

            start_alarm();

            record_event(
                EVENT_LOG_SCHEDULED_RING,
                (int32_t)ring_duration_ms
            );

            ESP_LOGI(
                TAG,
                "Scheduled alarm at %02u:%02u:%02u",
                current_time.hour,
                current_time.minute,
                current_time.second
            );

            last_display_second = -1;
        }

        if (
            alarm_active &&
            (int32_t)(
                now -
                alarm_end_tick
            ) >= 0
        )
        {
            stop_alarm();

            record_event(
                EVENT_LOG_ALARM_STOPPED,
                0
            );

            last_display_second = -1;
        }

        /* =================================================
         * Manual button
         * ================================================= */

        bool manual_pressed =
            gpio_get_level(
                MANUAL_BUTTON_PIN
            ) == 0;

        if (
            manual_pressed &&
            !previous_manual_pressed &&
            now -
                last_manual_event >
                pdMS_TO_TICKS(
                    BUTTON_DEBOUNCE_MS
                )
        )
        {
            last_manual_event = now;

            if (alarm_active)
            {
                stop_alarm();

                record_event(
                    EVENT_LOG_ALARM_STOPPED,
                    1
                );
            }
            else
            {
                start_alarm();

                record_event(
                    EVENT_LOG_MANUAL_RING,
                    (int32_t)ring_duration_ms
                );
            }

            last_display_second = -1;
        }

        previous_manual_pressed =
            manual_pressed;

        /* =================================================
         * Physical AUTO button
         * ================================================= */

        bool auto_pressed =
            gpio_get_level(
                AUTO_BUTTON_PIN
            ) == 0;

        if (
            auto_pressed &&
            !previous_auto_pressed &&
            now -
                last_auto_event >
                pdMS_TO_TICKS(
                    BUTTON_DEBOUNCE_MS
                )
        )
        {
            last_auto_event = now;

            apply_auto_state(
                !auto_enabled
            );

            last_display_second = -1;
        }

        previous_auto_pressed =
            auto_pressed;

        /* =================================================
         * LCD refresh
         * ================================================= */

        if (
            current_time.second !=
            last_display_second
        )
        {
            last_display_second =
                current_time.second;

            update_display(
                &current_time
            );
        }

        /*
         * Start the web server only after Wi-Fi has an IP.
         *
         * Failure does not affect RTC, timetable, relay,
         * keypad or manual bell operation.
         */
        if (
            wifi_manager_is_connected() &&
            !web_server_is_running() &&
            (int32_t)(
                now -
                next_web_server_retry_tick
            ) >= 0
        )
        {
            esp_err_t server_result =
                web_server_start(
                    provide_web_status,
                    &web_control_handlers
                );

            if (server_result == ESP_OK)
            {
                web_server_start_error_reported =
                    false;

                char web_ip[16];

                if (
                    wifi_manager_get_ip_string(
                        web_ip,
                        sizeof(web_ip)
                    ) != ESP_OK
                )
                {
                    snprintf(
                        web_ip,
                        sizeof(web_ip),
                        "0.0.0.0"
                    );
                }

                ESP_LOGI(
                    TAG,
                    "Read-only dashboard ready at http://%s/",
                    web_ip
                );
            }
            else
            {
                next_web_server_retry_tick =
                    now +
                    pdMS_TO_TICKS(
                        10000U
                    );

                if (
                    !web_server_start_error_reported
                )
                {
                    web_server_start_error_reported =
                        true;

                    ESP_LOGW(
                        TAG,
                        "Web server start failed: %s",
                        esp_err_to_name(
                            server_result
                        )
                    );

                    ESP_LOGW(
                        TAG,
                        "Offline alarm continues normally"
                    );
                }
            }
        }

        /*
         * Reaching this point proves that one complete main
         * loop cycle finished without becoming stuck.
         */
        if (system_watchdog_is_ready())
        {
            esp_err_t feed_result =
                system_watchdog_feed();

            if (feed_result == ESP_OK)
            {
                watchdog_feed_error_reported =
                    false;
            }
            else if (!watchdog_feed_error_reported)
            {
                watchdog_feed_error_reported =
                    true;

                ESP_LOGE(
                    TAG,
                    "Failed to feed task watchdog: %s",
                    esp_err_to_name(
                        feed_result
                    )
                );
            }
        }


        vTaskDelay(
            pdMS_TO_TICKS(50)
        );
    }
}
