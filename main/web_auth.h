#ifndef WEB_AUTH_H
#define WEB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WEB_AUTH_TOKEN_LENGTH 32
#define WEB_AUTH_TOKEN_BUFFER_SIZE 33

#define WEB_AUTH_MAX_SESSIONS 4
#define WEB_AUTH_SESSION_TIMEOUT_SECONDS 900U

typedef enum
{
    WEB_AUTH_RESULT_GRANTED = 0,
    WEB_AUTH_RESULT_DENIED,
    WEB_AUTH_RESULT_LOCKED,
    WEB_AUTH_RESULT_ERROR
} web_auth_result_t;

/*
 * Initialize browser-session authentication.
 *
 * Call this after access_control_init().
 */
esp_err_t web_auth_init(void);

/*
 * Verify the administrator PIN and create a session token.
 *
 * The token buffer must contain at least
 * WEB_AUTH_TOKEN_BUFFER_SIZE bytes.
 */
web_auth_result_t web_auth_login(
    const char *pin,
    char *token_buffer,
    size_t token_buffer_size,
    uint32_t *remaining_lockout_ms
);

/*
 * Validate and refresh an existing browser session.
 */
bool web_auth_validate(
    const char *token
);

/*
 * End one browser session.
 */
void web_auth_logout(
    const char *token
);

/*
 * End all active browser sessions.
 *
 * This is useful after changing the administrator PIN.
 */
void web_auth_logout_all(void);

#endif
