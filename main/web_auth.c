#include "web_auth.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "access_control.h"

/* =========================================================
 * Runtime configuration
 * ========================================================= */

#define WEB_AUTH_SESSION_TIMEOUT_MS \
    ((int64_t)WEB_AUTH_SESSION_TIMEOUT_SECONDS * 1000LL)

static const char *TAG =
    "WEB_AUTH";

/* =========================================================
 * Session storage
 * ========================================================= */

typedef struct
{
    bool active;

    char token[
        WEB_AUTH_TOKEN_BUFFER_SIZE
    ];

    int64_t expires_at_ms;
} web_auth_session_t;

static web_auth_session_t sessions[
    WEB_AUTH_MAX_SESSIONS
];

static StaticSemaphore_t
    auth_mutex_storage;

static SemaphoreHandle_t
    auth_mutex = NULL;

static bool auth_ready = false;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static int64_t current_time_ms(void)
{
    return
        esp_timer_get_time() /
        1000LL;
}

static void clear_session(
    web_auth_session_t *session
)
{
    if (session == NULL)
    {
        return;
    }

    memset(
        session,
        0,
        sizeof(*session)
    );
}

static bool token_has_valid_format(
    const char *token
)
{
    if (token == NULL)
    {
        return false;
    }

    for (size_t i = 0;
         i < WEB_AUTH_TOKEN_LENGTH;
         i++)
    {
        char character =
            token[i];

        bool is_number =
            character >= '0' &&
            character <= '9';

        bool is_lower_hex =
            character >= 'a' &&
            character <= 'f';

        if (!is_number &&
            !is_lower_hex)
        {
            return false;
        }
    }

    return
        token[WEB_AUTH_TOKEN_LENGTH] ==
        '\0';
}

/*
 * Constant-time comparison reduces information leakage
 * through token-comparison timing.
 */
static bool tokens_are_equal(
    const char *first,
    const char *second
)
{
    uint8_t difference = 0;

    for (size_t i = 0;
         i < WEB_AUTH_TOKEN_LENGTH;
         i++)
    {
        difference |=
            (uint8_t)(
                first[i] ^
                second[i]
            );
    }

    return difference == 0U;
}

static void generate_session_token(
    char *output
)
{
    static const char hex_digits[] =
        "0123456789abcdef";

    uint8_t random_bytes[16];

    esp_fill_random(
        random_bytes,
        sizeof(random_bytes)
    );

    for (size_t i = 0;
         i < sizeof(random_bytes);
         i++)
    {
        output[i * 2U] =
            hex_digits[
                random_bytes[i] >> 4U
            ];

        output[i * 2U + 1U] =
            hex_digits[
                random_bytes[i] & 0x0FU
            ];
    }

    output[WEB_AUTH_TOKEN_LENGTH] =
        '\0';

    /*
     * Remove the temporary random bytes from memory.
     */
    memset(
        random_bytes,
        0,
        sizeof(random_bytes)
    );
}

static void remove_expired_sessions(
    int64_t now_ms
)
{
    for (size_t i = 0;
         i < WEB_AUTH_MAX_SESSIONS;
         i++)
    {
        if (
            sessions[i].active &&
            now_ms >=
                sessions[i].expires_at_ms
        )
        {
            clear_session(
                &sessions[i]
            );
        }
    }
}

static web_auth_session_t *
find_session_for_token(
    const char *token
)
{
    for (size_t i = 0;
         i < WEB_AUTH_MAX_SESSIONS;
         i++)
    {
        if (
            sessions[i].active &&
            tokens_are_equal(
                sessions[i].token,
                token
            )
        )
        {
            return &sessions[i];
        }
    }

    return NULL;
}

static web_auth_session_t *
select_session_slot(void)
{
    /*
     * Prefer an unused session slot.
     */
    for (size_t i = 0;
         i < WEB_AUTH_MAX_SESSIONS;
         i++)
    {
        if (!sessions[i].active)
        {
            return &sessions[i];
        }
    }

    /*
     * When all slots are occupied, replace the session
     * that will expire first.
     */
    web_auth_session_t *oldest =
        &sessions[0];

    for (size_t i = 1;
         i < WEB_AUTH_MAX_SESSIONS;
         i++)
    {
        if (
            sessions[i].expires_at_ms <
            oldest->expires_at_ms
        )
        {
            oldest =
                &sessions[i];
        }
    }

    return oldest;
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t web_auth_init(void)
{
    if (auth_ready)
    {
        return ESP_OK;
    }

    auth_mutex =
        xSemaphoreCreateMutexStatic(
            &auth_mutex_storage
        );

    if (auth_mutex == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create authentication mutex"
        );

        return ESP_ERR_NO_MEM;
    }

    memset(
        sessions,
        0,
        sizeof(sessions)
    );

    auth_ready = true;

    ESP_LOGI(
        TAG,
        "Web authentication initialized"
    );

    return ESP_OK;
}

web_auth_result_t web_auth_login(
    const char *pin,
    char *token_buffer,
    size_t token_buffer_size,
    uint32_t *remaining_lockout_ms
)
{
    if (remaining_lockout_ms != NULL)
    {
        *remaining_lockout_ms = 0;
    }

    if (token_buffer != NULL &&
        token_buffer_size > 0U)
    {
        token_buffer[0] = '\0';
    }

    if (
        !auth_ready ||
        pin == NULL ||
        token_buffer == NULL ||
        token_buffer_size <
            WEB_AUTH_TOKEN_BUFFER_SIZE
    )
    {
        return WEB_AUTH_RESULT_ERROR;
    }

    uint32_t now_for_access_control =
        (uint32_t)current_time_ms();

    access_result_t access_result =
        access_control_verify(
            pin,
            now_for_access_control,
            remaining_lockout_ms
        );

    if (access_result ==
        ACCESS_RESULT_DENIED)
    {
        return WEB_AUTH_RESULT_DENIED;
    }

    if (access_result ==
        ACCESS_RESULT_LOCKED)
    {
        return WEB_AUTH_RESULT_LOCKED;
    }

    if (access_result !=
        ACCESS_RESULT_GRANTED)
    {
        return WEB_AUTH_RESULT_ERROR;
    }

    if (
        xSemaphoreTake(
            auth_mutex,
            portMAX_DELAY
        ) != pdTRUE
    )
    {
        return WEB_AUTH_RESULT_ERROR;
    }

    int64_t now_ms =
        current_time_ms();

    remove_expired_sessions(
        now_ms
    );

    web_auth_session_t *session =
        select_session_slot();

    clear_session(
        session
    );

    generate_session_token(
        session->token
    );

    session->expires_at_ms =
        now_ms +
        WEB_AUTH_SESSION_TIMEOUT_MS;

    session->active = true;

    memcpy(
        token_buffer,
        session->token,
        WEB_AUTH_TOKEN_BUFFER_SIZE
    );

    xSemaphoreGive(
        auth_mutex
    );

    ESP_LOGI(
        TAG,
        "Administrator web session created"
    );

    return WEB_AUTH_RESULT_GRANTED;
}

bool web_auth_validate(
    const char *token
)
{
    if (
        !auth_ready ||
        !token_has_valid_format(token)
    )
    {
        return false;
    }

    if (
        xSemaphoreTake(
            auth_mutex,
            portMAX_DELAY
        ) != pdTRUE
    )
    {
        return false;
    }

    int64_t now_ms =
        current_time_ms();

    remove_expired_sessions(
        now_ms
    );

    web_auth_session_t *session =
        find_session_for_token(
            token
        );

    bool valid =
        session != NULL;

    if (valid)
    {
        /*
         * Refresh the session after authenticated activity.
         */
        session->expires_at_ms =
            now_ms +
            WEB_AUTH_SESSION_TIMEOUT_MS;
    }

    xSemaphoreGive(
        auth_mutex
    );

    return valid;
}

void web_auth_logout(
    const char *token
)
{
    if (
        !auth_ready ||
        !token_has_valid_format(token)
    )
    {
        return;
    }

    if (
        xSemaphoreTake(
            auth_mutex,
            portMAX_DELAY
        ) != pdTRUE
    )
    {
        return;
    }

    web_auth_session_t *session =
        find_session_for_token(
            token
        );

    if (session != NULL)
    {
        clear_session(
            session
        );

        ESP_LOGI(
            TAG,
            "Administrator web session ended"
        );
    }

    xSemaphoreGive(
        auth_mutex
    );
}

void web_auth_logout_all(void)
{
    if (!auth_ready)
    {
        return;
    }

    if (
        xSemaphoreTake(
            auth_mutex,
            portMAX_DELAY
        ) != pdTRUE
    )
    {
        return;
    }

    memset(
        sessions,
        0,
        sizeof(sessions)
    );

    xSemaphoreGive(
        auth_mutex
    );

    ESP_LOGI(
        TAG,
        "All administrator web sessions ended"
    );
}
