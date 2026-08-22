#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "web_auth.h"
#include "access_control.h"

/* =========================================================
 * Configuration
 * ========================================================= */

#define AUTH_COOKIE_NAME "alarm_session"
#define MAX_COOKIE_HEADER_LENGTH 256
#define MAX_LOGIN_BODY_LENGTH 64

static const char *TAG =
    "WEB_SERVER";

/* =========================================================
 * Runtime state
 * ========================================================= */

static httpd_handle_t server_handle =
    NULL;

static web_status_provider_t
    current_status_provider =
        NULL;

/* =========================================================
 * HTML pages
 * ========================================================= */

static const char dashboard_html[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" "
"content=\"width=device-width,initial-scale=1\">"
"<title>Smart School Alarm</title>"
"<style>"
"body{font-family:Arial,sans-serif;"
"background:#f2f5f8;margin:0;padding:20px;"
"color:#18202a}"
".container{max-width:980px;margin:auto}"
".header{background:#123b68;color:white;"
"padding:18px 22px;border-radius:8px;"
"display:flex;align-items:center;"
"justify-content:space-between;gap:12px}"
"h1{margin:0}"
".button{display:inline-block;background:white;"
"color:#123b68;text-decoration:none;"
"padding:10px 15px;border-radius:6px;"
"font-weight:bold}"
".grid{display:grid;"
"grid-template-columns:repeat(auto-fit,"
"minmax(210px,1fr));gap:12px;margin-top:26px}"
".card{background:white;padding:16px;"
"border-radius:8px;"
"box-shadow:0 2px 8px rgba(0,0,0,.1)}"
".label{font-size:13px;color:#667}"
".value{font-size:20px;font-weight:bold;"
"margin-top:6px}"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<div class=\"header\">"
"<h1>Smart School Alarm</h1>"
"<a class=\"button\" href=\"/admin\">Admin Login</a>"
"</div>"
"<div class=\"grid\">"

"<div class=\"card\">"
"<div class=\"label\">Current time</div>"
"<div class=\"value\" id=\"time\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Next bell</div>"
"<div class=\"value\" id=\"next\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Automatic mode</div>"
"<div class=\"value\" id=\"auto\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Bell output</div>"
"<div class=\"value\" id=\"alarm\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">RTC</div>"
"<div class=\"value\" id=\"rtc\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Wi-Fi</div>"
"<div class=\"value\" id=\"wifi\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">IP address</div>"
"<div class=\"value\" id=\"ip\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Timetable entries</div>"
"<div class=\"value\" id=\"bells\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Stored events</div>"
"<div class=\"value\" id=\"logs\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Ring duration</div>"
"<div class=\"value\" id=\"duration\">Loading...</div>"
"</div>"

"</div>"
"</div>"

"<script>"
"async function updateStatus(){"
"try{"
"const response=await fetch('/api/status');"
"const data=await response.json();"

"document.getElementById('time').textContent="
"data.current_time;"

"document.getElementById('next').textContent="
"data.next_bell;"

"document.getElementById('auto').textContent="
"data.auto_enabled?'ENABLED':'DISABLED';"

"document.getElementById('alarm').textContent="
"data.alarm_active?'RINGING':'OFF';"

"document.getElementById('rtc').textContent="
"data.rtc_ready?'READY':'FAULT';"

"document.getElementById('wifi').textContent="
"data.wifi_state;"

"document.getElementById('ip').textContent="
"data.ip_address;"

"document.getElementById('bells').textContent="
"data.timetable_count;"

"document.getElementById('logs').textContent="
"data.event_log_count;"

"document.getElementById('duration').textContent="
"data.ring_duration_seconds+' seconds';"
"}catch(error){"
"document.getElementById('wifi').textContent="
"'SERVER UNAVAILABLE';"
"}"
"}"
"updateStatus();"
"setInterval(updateStatus,2000);"
"</script>"
"</body>"
"</html>";

static const char login_html[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" "
"content=\"width=device-width,initial-scale=1\">"
"<title>Administrator Login</title>"
"<style>"
"body{font-family:Arial,sans-serif;"
"background:#eef3f8;margin:0;padding:24px;"
"color:#17202a}"
".box{max-width:420px;margin:80px auto;"
"background:white;padding:28px;"
"border-radius:10px;"
"box-shadow:0 4px 18px rgba(0,0,0,.12)}"
"h1{margin-top:0;color:#123b68}"
"label{display:block;margin-bottom:8px;"
"font-weight:bold}"
"input{width:100%;box-sizing:border-box;"
"padding:13px;border:1px solid #bbc4ce;"
"border-radius:6px;font-size:18px}"
"button{width:100%;margin-top:16px;"
"padding:13px;border:0;border-radius:6px;"
"background:#123b68;color:white;"
"font-size:16px;font-weight:bold;cursor:pointer}"
"a{display:block;text-align:center;"
"margin-top:18px;color:#123b68}"
"</style>"
"</head>"
"<body>"
"<div class=\"box\">"
"<h1>Administrator Login</h1>"
"<p>Enter the same administrator PIN used by the keypad.</p>"
"<form method=\"POST\" action=\"/api/login\">"
"<label for=\"pin\">Administrator PIN</label>"
"<input id=\"pin\" name=\"pin\" type=\"password\" "
"inputmode=\"numeric\" minlength=\"4\" maxlength=\"8\" "
"pattern=\"[0-9]{4,8}\" required autofocus>"
"<button type=\"submit\">Login</button>"
"</form>"
"<a href=\"/\">Return to dashboard</a>"
"</div>"
"</body>"
"</html>";

static const char login_failed_html[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" "
"content=\"width=device-width,initial-scale=1\">"
"<title>Login Failed</title>"
"<style>"
"body{font-family:Arial;background:#eef3f8;"
"padding:24px;color:#17202a}"
".box{max-width:420px;margin:80px auto;"
"background:white;padding:28px;border-radius:10px;"
"box-shadow:0 4px 18px rgba(0,0,0,.12)}"
"h1{color:#b42318}"
"a{display:inline-block;margin-top:14px;"
"color:#123b68;font-weight:bold}"
"</style>"
"</head>"
"<body>"
"<div class=\"box\">"
"<h1>Incorrect PIN</h1>"
"<p>Administrator access was denied.</p>"
"<a href=\"/login\">Try again</a>"
"</div>"
"</body>"
"</html>";

static const char admin_html[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" "
"content=\"width=device-width,initial-scale=1\">"
"<title>Alarm Administration</title>"
"<style>"
"body{font-family:Arial;background:#eef3f8;"
"margin:0;padding:24px;color:#17202a}"
".container{max-width:820px;margin:auto}"
".header{background:#123b68;color:white;"
"padding:20px;border-radius:9px;"
"display:flex;justify-content:space-between;"
"align-items:center;gap:12px}"
"h1{margin:0}"
".button{background:white;color:#123b68;"
"text-decoration:none;padding:10px 14px;"
"border-radius:6px;font-weight:bold}"
".card{background:white;margin-top:22px;"
"padding:24px;border-radius:9px;"
"box-shadow:0 3px 14px rgba(0,0,0,.1)}"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<div class=\"header\">"
"<h1>Alarm Administration</h1>"
"<a class=\"button\" href=\"/logout\">Logout</a>"
"</div>"
"<div class=\"card\">"
"<h2>Administrator authenticated</h2>"
"<p>This protected area is ready.</p>"
"<p>The next stage will add validated timetable, "
"AUTO-mode and ring-duration controls here.</p>"
"<p><a href=\"/\">Open read-only dashboard</a></p>"
"</div>"
"</div>"
"</body>"
"</html>";

/* =========================================================
 * HTTP helpers
 * ========================================================= */

static esp_err_t send_html(
    httpd_req_t *request,
    const char *html
)
{
    httpd_resp_set_type(
        request,
        "text/html"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        html,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t send_html_status(
    httpd_req_t *request,
    const char *status,
    const char *html
)
{
    httpd_resp_set_status(
        request,
        status
    );

    return send_html(
        request,
        html
    );
}

static esp_err_t redirect_to(
    httpd_req_t *request,
    const char *location
)
{
    httpd_resp_set_status(
        request,
        "302 Found"
    );

    httpd_resp_set_hdr(
        request,
        "Location",
        location
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        NULL,
        0
    );
}

static bool extract_session_token(
    httpd_req_t *request,
    char *token
)
{
    if (request == NULL ||
        token == NULL)
    {
        return false;
    }

    token[0] = '\0';

    size_t header_length =
        httpd_req_get_hdr_value_len(
            request,
            "Cookie"
        );

    if (header_length == 0U ||
        header_length >=
            MAX_COOKIE_HEADER_LENGTH)
    {
        return false;
    }

    char cookies[
        MAX_COOKIE_HEADER_LENGTH
    ];

    if (
        httpd_req_get_hdr_value_str(
            request,
            "Cookie",
            cookies,
            sizeof(cookies)
        ) != ESP_OK
    )
    {
        return false;
    }

    const char cookie_prefix[] =
        AUTH_COOKIE_NAME "=";

    const size_t prefix_length =
        sizeof(cookie_prefix) - 1U;

    const char *position =
        cookies;

    while (*position != '\0')
    {
        while (
            *position == ' ' ||
            *position == ';'
        )
        {
            position++;
        }

        if (
            strncmp(
                position,
                cookie_prefix,
                prefix_length
            ) == 0
        )
        {
            position +=
                prefix_length;

            size_t token_length = 0;

            while (
                position[token_length] != '\0' &&
                position[token_length] != ';'
            )
            {
                token_length++;
            }

            if (token_length !=
                WEB_AUTH_TOKEN_LENGTH)
            {
                return false;
            }

            memcpy(
                token,
                position,
                WEB_AUTH_TOKEN_LENGTH
            );

            token[
                WEB_AUTH_TOKEN_LENGTH
            ] = '\0';

            return true;
        }

        const char *next_cookie =
            strchr(
                position,
                ';'
            );

        if (next_cookie == NULL)
        {
            break;
        }

        position =
            next_cookie + 1;
    }

    return false;
}

static bool request_is_authenticated(
    httpd_req_t *request
)
{
    char token[
        WEB_AUTH_TOKEN_BUFFER_SIZE
    ];

    if (!extract_session_token(
            request,
            token
        ))
    {
        return false;
    }

    return web_auth_validate(
        token
    );
}

static bool read_login_pin(
    httpd_req_t *request,
    char *pin,
    size_t pin_size
)
{
    if (request == NULL ||
        pin == NULL ||
        pin_size == 0U)
    {
        return false;
    }

    if (
        request->content_len == 0 ||
        request->content_len >=
            MAX_LOGIN_BODY_LENGTH
    )
    {
        return false;
    }

    char body[
        MAX_LOGIN_BODY_LENGTH
    ];

    size_t received_total = 0;

    while (
        received_total <
        request->content_len
    )
    {
        int received =
            httpd_req_recv(
                request,
                body + received_total,
                request->content_len -
                    received_total
            );

        if (received <= 0)
        {
            return false;
        }

        received_total +=
            (size_t)received;
    }

    body[received_total] =
        '\0';

    const char prefix[] =
        "pin=";

    if (
        strncmp(
            body,
            prefix,
            sizeof(prefix) - 1U
        ) != 0
    )
    {
        return false;
    }

    const char *entered_pin =
        body + sizeof(prefix) - 1U;

    size_t pin_length =
        strlen(entered_pin);

    if (
        pin_length <
            ACCESS_PIN_MIN_LENGTH ||
        pin_length >
            ACCESS_PIN_MAX_LENGTH ||
        pin_length >= pin_size
    )
    {
        return false;
    }

    for (size_t i = 0;
         i < pin_length;
         i++)
    {
        if (
            entered_pin[i] < '0' ||
            entered_pin[i] > '9'
        )
        {
            return false;
        }
    }

    memcpy(
        pin,
        entered_pin,
        pin_length + 1U
    );

    return true;
}

/* =========================================================
 * Dashboard and status handlers
 * ========================================================= */

static esp_err_t dashboard_handler(
    httpd_req_t *request
)
{
    return send_html(
        request,
        dashboard_html
    );
}

static esp_err_t status_handler(
    httpd_req_t *request
)
{
    web_status_t status;

    memset(
        &status,
        0,
        sizeof(status)
    );

    snprintf(
        status.current_time,
        sizeof(status.current_time),
        "Unavailable"
    );

    snprintf(
        status.next_bell,
        sizeof(status.next_bell),
        "Unavailable"
    );

    snprintf(
        status.ip_address,
        sizeof(status.ip_address),
        "0.0.0.0"
    );

    snprintf(
        status.wifi_state,
        sizeof(status.wifi_state),
        "DISCONNECTED"
    );

    if (current_status_provider != NULL)
    {
        current_status_provider(
            &status
        );
    }

    char json[640];

    int written =
        snprintf(
            json,
            sizeof(json),

            "{"
            "\"auto_enabled\":%s,"
            "\"alarm_active\":%s,"
            "\"rtc_ready\":%s,"
            "\"wifi_connected\":%s,"
            "\"timetable_count\":%lu,"
            "\"event_log_count\":%lu,"
            "\"ring_duration_seconds\":%lu,"
            "\"uptime_seconds\":%lu,"
            "\"current_time\":\"%s\","
            "\"next_bell\":\"%s\","
            "\"ip_address\":\"%s\","
            "\"wifi_state\":\"%s\""
            "}",

            status.auto_enabled
                ? "true"
                : "false",

            status.alarm_active
                ? "true"
                : "false",

            status.rtc_ready
                ? "true"
                : "false",

            status.wifi_connected
                ? "true"
                : "false",

            (unsigned long)
                status.timetable_count,

            (unsigned long)
                status.event_log_count,

            (unsigned long)
                status.ring_duration_seconds,

            (unsigned long)
                status.uptime_seconds,

            status.current_time,
            status.next_bell,
            status.ip_address,
            status.wifi_state
        );

    if (written < 0 ||
        written >= (int)sizeof(json))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Unable to generate status"
        );
    }

    httpd_resp_set_type(
        request,
        "application/json"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        json,
        written
    );
}

/* =========================================================
 * Authentication handlers
 * ========================================================= */

static esp_err_t login_page_handler(
    httpd_req_t *request
)
{
    if (request_is_authenticated(
            request
        ))
    {
        return redirect_to(
            request,
            "/admin"
        );
    }

    return send_html(
        request,
        login_html
    );
}

static esp_err_t login_handler(
    httpd_req_t *request
)
{
    char pin[
        ACCESS_PIN_MAX_LENGTH + 1
    ];

    if (!read_login_pin(
            request,
            pin,
            sizeof(pin)
        ))
    {
        return send_html_status(
            request,
            "400 Bad Request",
            login_failed_html
        );
    }

    char token[
        WEB_AUTH_TOKEN_BUFFER_SIZE
    ];

    uint32_t remaining_lockout_ms =
        0;

    web_auth_result_t result =
        web_auth_login(
            pin,
            token,
            sizeof(token),
            &remaining_lockout_ms
        );

    memset(
        pin,
        0,
        sizeof(pin)
    );

    if (result ==
        WEB_AUTH_RESULT_GRANTED)
    {
        char cookie_header[160];

        int cookie_length =
            snprintf(
                cookie_header,
                sizeof(cookie_header),

                AUTH_COOKIE_NAME
                "=%.*s; Path=/; HttpOnly; "
                "SameSite=Strict; Max-Age=%u",

                WEB_AUTH_TOKEN_LENGTH,
                token,

                (unsigned int)
                    WEB_AUTH_SESSION_TIMEOUT_SECONDS
            );

        memset(
            token,
            0,
            sizeof(token)
        );

        if (
            cookie_length < 0 ||
            cookie_length >=
                (int)sizeof(cookie_header)
        )
        {
            return httpd_resp_send_err(
                request,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to create session"
            );
        }

        httpd_resp_set_hdr(
            request,
            "Set-Cookie",
            cookie_header
        );

        return redirect_to(
            request,
            "/admin"
        );
    }

    memset(
        token,
        0,
        sizeof(token)
    );

    if (result ==
        WEB_AUTH_RESULT_LOCKED)
    {
        uint32_t seconds =
            (
                remaining_lockout_ms +
                999U
            ) /
            1000U;

        char locked_page[900];

        int written =
            snprintf(
                locked_page,
                sizeof(locked_page),

                "<!DOCTYPE html>"
                "<html lang=\"en\">"
                "<head>"
                "<meta charset=\"UTF-8\">"
                "<meta name=\"viewport\" "
                "content=\"width=device-width,"
                "initial-scale=1\">"
                "<title>Access Locked</title>"
                "<style>"
                "body{font-family:Arial;"
                "background:#eef3f8;padding:24px}"
                ".box{max-width:420px;margin:80px auto;"
                "background:white;padding:28px;"
                "border-radius:10px;"
                "box-shadow:0 4px 18px rgba(0,0,0,.12)}"
                "h1{color:#b42318}"
                "a{color:#123b68;font-weight:bold}"
                "</style>"
                "</head>"
                "<body><div class=\"box\">"
                "<h1>Access Locked</h1>"
                "<p>Too many incorrect PIN attempts.</p>"
                "<p>Try again in approximately %lu seconds.</p>"
                "<a href=\"/login\">Return to login</a>"
                "</div></body></html>",

                (unsigned long)seconds
            );

        if (
            written < 0 ||
            written >=
                (int)sizeof(locked_page)
        )
        {
            return httpd_resp_send_err(
                request,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Unable to show lockout"
            );
        }

        return send_html_status(
            request,
            "423 Locked",
            locked_page
        );
    }

    if (result ==
        WEB_AUTH_RESULT_DENIED)
    {
        return send_html_status(
            request,
            "401 Unauthorized",
            login_failed_html
        );
    }

    return httpd_resp_send_err(
        request,
        HTTPD_500_INTERNAL_SERVER_ERROR,
        "Authentication unavailable"
    );
}

static esp_err_t admin_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return redirect_to(
            request,
            "/login"
        );
    }

    return send_html(
        request,
        admin_html
    );
}

static esp_err_t logout_handler(
    httpd_req_t *request
)
{
    char token[
        WEB_AUTH_TOKEN_BUFFER_SIZE
    ];

    if (extract_session_token(
            request,
            token
        ))
    {
        web_auth_logout(
            token
        );

        memset(
            token,
            0,
            sizeof(token)
        );
    }

    httpd_resp_set_hdr(
        request,
        "Set-Cookie",

        AUTH_COOKIE_NAME
        "=; Path=/; HttpOnly; "
        "SameSite=Strict; Max-Age=0"
    );

    return redirect_to(
        request,
        "/login"
    );
}

/* =========================================================
 * Public functions
 * ========================================================= */

esp_err_t web_server_start(
    web_status_provider_t status_provider
)
{
    if (server_handle != NULL)
    {
        return ESP_OK;
    }

    if (status_provider == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t auth_result =
        web_auth_init();

    if (auth_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Web authentication initialization failed: %s",
            esp_err_to_name(auth_result)
        );

        return auth_result;
    }

    current_status_provider =
        status_provider;

    httpd_config_t configuration =
        HTTPD_DEFAULT_CONFIG();

    configuration.server_port = 80;
    configuration.max_uri_handlers = 8;
    configuration.stack_size = 7168;

    esp_err_t result =
        httpd_start(
            &server_handle,
            &configuration
        );

    if (result != ESP_OK)
    {
        server_handle = NULL;
        current_status_provider = NULL;

        ESP_LOGE(
            TAG,
            "HTTP server start failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    const httpd_uri_t handlers[] = {
        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = dashboard_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/login",
            .method = HTTP_GET,
            .handler = login_page_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/login",
            .method = HTTP_POST,
            .handler = login_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/admin",
            .method = HTTP_GET,
            .handler = admin_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/logout",
            .method = HTTP_GET,
            .handler = logout_handler,
            .user_ctx = NULL
        }
    };

    const size_t handler_count =
        sizeof(handlers) /
        sizeof(handlers[0]);

    for (size_t i = 0;
         i < handler_count;
         i++)
    {
        result =
            httpd_register_uri_handler(
                server_handle,
                &handlers[i]
            );

        if (result != ESP_OK)
        {
            web_server_stop();
            return result;
        }
    }

    ESP_LOGI(
        TAG,
        "Authenticated HTTP server started"
    );

    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server_handle == NULL)
    {
        return ESP_OK;
    }

    esp_err_t result =
        httpd_stop(
            server_handle
        );

    server_handle = NULL;
    current_status_provider = NULL;

    return result;
}

bool web_server_is_running(void)
{
    return server_handle != NULL;
}

