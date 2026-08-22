#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

/* =========================================================
 * Runtime state
 * ========================================================= */

static const char *TAG =
    "WEB_SERVER";

static httpd_handle_t server_handle =
    NULL;

static web_status_provider_t
    current_status_provider =
        NULL;

/* =========================================================
 * Dashboard page
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
".container{max-width:760px;margin:auto}"
"h1{background:#123b68;color:white;"
"padding:18px;border-radius:8px}"
".grid{display:grid;"
"grid-template-columns:repeat(auto-fit,"
"minmax(210px,1fr));gap:12px}"
".card{background:white;padding:16px;"
"border-radius:8px;"
"box-shadow:0 2px 8px rgba(0,0,0,.1)}"
".label{font-size:13px;color:#667}"
".value{font-size:20px;font-weight:bold;"
"margin-top:6px}"
".ok{color:#087f23}"
".off{color:#b42318}"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<h1>Smart School Alarm</h1>"
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

/* =========================================================
 * HTTP handlers
 * ========================================================= */

static esp_err_t dashboard_handler(
    httpd_req_t *request
)
{
    httpd_resp_set_type(
        request,
        "text/html"
    );

    return httpd_resp_send(
        request,
        dashboard_html,
        HTTPD_RESP_USE_STRLEN
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
        ESP_LOGE(
            TAG,
            "Status JSON buffer is too small"
        );

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

    current_status_provider =
        status_provider;

    httpd_config_t configuration =
        HTTPD_DEFAULT_CONFIG();

    configuration.server_port = 80;

    configuration.max_uri_handlers = 4;

    configuration.stack_size = 6144;

    esp_err_t result =
        httpd_start(
            &server_handle,
            &configuration
        );

    if (result != ESP_OK)
    {
        server_handle = NULL;

        current_status_provider =
            NULL;

        ESP_LOGE(
            TAG,
            "HTTP server start failed: %s",
            esp_err_to_name(result)
        );

        return result;
    }

    httpd_uri_t dashboard_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dashboard_handler,
        .user_ctx = NULL
    };

    result =
        httpd_register_uri_handler(
            server_handle,
            &dashboard_uri
        );

    if (result != ESP_OK)
    {
        web_server_stop();
        return result;
    }

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };

    result =
        httpd_register_uri_handler(
            server_handle,
            &status_uri
        );

    if (result != ESP_OK)
    {
        web_server_stop();
        return result;
    }

    ESP_LOGI(
        TAG,
        "Read-only HTTP server started"
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

    current_status_provider =
        NULL;

    return result;
}

bool web_server_is_running(void)
{
    return server_handle != NULL;
}
