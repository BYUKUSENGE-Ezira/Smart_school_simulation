#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "web_auth.h"
#include "web_timetable_bridge.h"
#include "web_event_bridge.h"
#include "access_control.h"

/* =========================================================
 * Configuration
 * ========================================================= */

#define AUTH_COOKIE_NAME "alarm_session"
#define MAX_COOKIE_HEADER_LENGTH 256
#define MAX_LOGIN_BODY_LENGTH 64
#define MAX_ADMIN_FORM_BODY_LENGTH 128

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

static web_control_handlers_t
    current_control_handlers = {0};

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
"<div class=\"label\">Announcement mode</div>"
"<div class=\"value\" id=\"announcementState\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">PA announcement output</div>"
"<div class=\"value\" id=\"announcementOutput\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Bell blocked by announcement</div>"
"<div class=\"value\" id=\"announcementBlocked\">"
"Loading..."
"</div>"
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
"<div class=\"label\">MQTT</div>"
"<div class=\"value\" id=\"mqtt\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">MQTT topic prefix</div>"
"<div class=\"value\" id=\"mqttTopic\" "
"style=\"font-size:14px;word-break:break-all\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">MQTT broker</div>"
"<div class=\"value\" id=\"mqttBroker\" "
"style=\"font-size:14px;word-break:break-all\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">MQTT diagnostic</div>"
"<div class=\"value\" id=\"mqttDiagnostic\" "
"style=\"font-size:14px;word-break:break-word\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">MQTT disconnect count</div>"
"<div class=\"value\" id=\"mqttDisconnects\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Timetable storage</div>"
"<div class=\"value\" id=\"bells\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Event-log storage</div>"
"<div class=\"value\" id=\"logs\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Ring duration</div>"
"<div class=\"value\" id=\"duration\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">NVS storage</div>"
"<div class=\"value\" id=\"nvsState\">Loading...</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">NVS entries</div>"
"<div class=\"value\" id=\"nvsEntries\" "
"style=\"font-size:16px\">"
"Loading..."
"</div>"
"</div>"

"<div class=\"card\">"
"<div class=\"label\">Application partition</div>"
"<div class=\"value\" id=\"appPartition\">"
"Loading..."
"</div>"
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

"document.getElementById('announcementState').textContent="
"data.announcement_state||'UNAVAILABLE';"

"document.getElementById('announcementOutput').textContent="
"data.announcement_pa_active?'ON':'OFF';"

"document.getElementById('announcementBlocked').textContent="
"data.bell_blocked_by_announcement?'YES':'NO';"

"document.getElementById('rtc').textContent="
"data.rtc_ready?'READY':'FAULT';"

"document.getElementById('wifi').textContent="
"data.wifi_state;"

"document.getElementById('ip').textContent="
"data.ip_address;"

"document.getElementById('mqtt').textContent="
"data.mqtt_connected?'CONNECTED':'DISCONNECTED';"

"document.getElementById('mqttTopic').textContent="
"data.mqtt_topic||'Unavailable';"

"document.getElementById('mqttBroker').textContent="
"data.mqtt_broker||'Unavailable';"

"document.getElementById('mqttDiagnostic').textContent="
"data.mqtt_diagnostic||'No diagnostic information';"

"document.getElementById('mqttDisconnects').textContent="
"data.mqtt_disconnect_count;"

"document.getElementById('bells').textContent="
"data.timetable_count+' / '+data.timetable_capacity;"

"document.getElementById('logs').textContent="
"data.event_log_count+' / '+data.event_log_capacity;"

"document.getElementById('duration').textContent="
"data.ring_duration_seconds+' seconds';"

"document.getElementById('nvsState').textContent="
"data.nvs_ready?'READY':'FAULT';"

"document.getElementById('nvsEntries').textContent="
"data.nvs_used_entries+' used / '+"
"data.nvs_free_entries+' free / '+"
"data.nvs_total_entries+' total';"

"document.getElementById('appPartition').textContent="
"(data.app_partition_size_bytes/1048576)"
".toFixed(2)+' MB';"
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
"body{font-family:Arial,sans-serif;"
"background:#eef3f8;margin:0;padding:24px;"
"color:#17202a}"
".container{max-width:1050px;margin:auto}"
".header{background:#123b68;color:white;"
"padding:20px;border-radius:9px;"
"display:flex;justify-content:space-between;"
"align-items:center;gap:12px}"
"h1{margin:0}"
".header a{background:white;color:#123b68;"
"text-decoration:none;padding:10px 14px;"
"border-radius:6px;font-weight:bold}"
".card{background:white;margin-top:20px;"
"padding:22px;border-radius:9px;"
"box-shadow:0 3px 14px rgba(0,0,0,.1)}"
".grid{display:grid;"
"grid-template-columns:repeat(auto-fit,minmax(280px,1fr));"
"gap:18px}"
"label{display:block;margin:10px 0 6px;"
"font-weight:bold}"
"input,select{width:100%;box-sizing:border-box;"
"padding:10px;border:1px solid #b8c2cc;"
"border-radius:5px;font-size:16px}"
"button{padding:10px 14px;margin:6px 4px 6px 0;"
"border:0;border-radius:5px;cursor:pointer;"
"font-weight:bold;background:#123b68;color:white}"
".danger{background:#b42318}"
".secondary{background:#536779}"
".message{margin-top:12px;padding:10px;"
"border-radius:5px;background:#edf4ff;display:none}"
".table-wrap{overflow-x:auto}"
"table{width:100%;border-collapse:collapse;margin-top:14px}"
"th,td{text-align:left;padding:10px;"
"border-bottom:1px solid #dde3e8}"
"th{background:#f4f6f8}"
".small{font-size:13px;color:#5d6975}"
"</style>"
"</head>"

"<body>"
"<div class=\"container\">"

"<div class=\"header\">"
"<h1>Alarm Administration</h1>"
"<a href=\"/logout\">Logout</a>"
"</div>"

"<div class=\"grid\">"

"<div class=\"card\">"
"<h2>Automatic mode</h2>"
"<p>Current state: "
"<strong id=\"autoState\">Loading...</strong></p>"

"<form method=\"POST\" action=\"/api/admin/auto\">"
"<button type=\"submit\" name=\"enabled\" value=\"1\">"
"Enable AUTO"
"</button>"
"<button class=\"secondary\" type=\"submit\" "
"name=\"enabled\" value=\"0\">"
"Disable AUTO"
"</button>"
"</form>"
"</div>"

"<div class=\"card\">"
"<h2>Ring duration</h2>"
"<p>Current duration: "
"<strong id=\"durationState\">Loading...</strong></p>"

"<form method=\"POST\" action=\"/api/admin/duration\">"
"<label for=\"seconds\">Seconds</label>"
"<input id=\"seconds\" name=\"seconds\" type=\"number\" "
"min=\"1\" max=\"60\" required>"
"<button type=\"submit\">Save duration</button>"
"</form>"
"</div>"

"</div>"

"<div class=\"card\">"
"<h2>Add timetable bell</h2>"
"<p class=\"small\">"
"RTC weekday numbering: Sunday=1, Monday=2, "
"Tuesday=3, Wednesday=4, Thursday=5, "
"Friday=6, Saturday=7."
"</p>"

"<form id=\"addBellForm\">"

"<label for=\"weekday\">Weekday</label>"
"<select id=\"weekday\" required>"
"<option value=\"1\">Sunday</option>"
"<option value=\"2\" selected>Monday</option>"
"<option value=\"3\">Tuesday</option>"
"<option value=\"4\">Wednesday</option>"
"<option value=\"5\">Thursday</option>"
"<option value=\"6\">Friday</option>"
"<option value=\"7\">Saturday</option>"
"</select>"

"<label for=\"hour\">Hour: 0-23</label>"
"<input id=\"hour\" type=\"number\" "
"min=\"0\" max=\"23\" value=\"8\" required>"

"<label for=\"minute\">Minute: 0-59</label>"
"<input id=\"minute\" type=\"number\" "
"min=\"0\" max=\"59\" value=\"0\" required>"

"<button type=\"submit\">Add bell</button>"
"</form>"

"<div id=\"message\" class=\"message\"></div>"
"</div>"

"<div class=\"card\">"
"<h2>Saved timetable</h2>"
"<p><strong id=\"bellCount\">Loading...</strong></p>"

"<button class=\"secondary\" type=\"button\" "
"onclick=\"loadTimetable()\">Refresh</button>"

"<button class=\"danger\" type=\"button\" "
"onclick=\"restoreDefaults()\">"
"Restore 75 default bells"
"</button>"

"<div class=\"table-wrap\">"
"<table>"
"<thead>"
"<tr>"
"<th>#</th>"
"<th>Weekday</th>"
"<th>Time</th>"
"<th>Action</th>"
"</tr>"
"</thead>"
"<tbody id=\"timetableBody\"></tbody>"
"</table>"
"</div>"
"</div>"

"<div class=\"card\">"
"<h2>Event log</h2>"

"<p><strong id=\"eventCount\">Loading...</strong></p>"

"<button class=\"secondary\" type=\"button\" "
"onclick=\"loadEvents()\">"
"Refresh events"
"</button>"

"<button class=\"danger\" type=\"button\" "
"onclick=\"clearEvents()\">"
"Clear event log"
"</button>"

"<div class=\"table-wrap\">"
"<table>"
"<thead>"
"<tr>"
"<th>Sequence</th>"
"<th>Timestamp</th>"
"<th>Event</th>"
"<th>Value</th>"
"</tr>"
"</thead>"
"<tbody id=\"eventBody\"></tbody>"
"</table>"
"</div>"
"</div>"

"<div class=\"card\">"
"<a href=\"/\">Open read-only dashboard</a>"
"</div>"

"</div>"

"<script>"
"const dayNames=["
"'','Sunday','Monday','Tuesday','Wednesday',"
"'Thursday','Friday','Saturday'];"

"function showMessage(text,isError=false){"
"const box=document.getElementById('message');"
"box.textContent=text;"
"box.style.display='block';"
"box.style.background=isError?'#feeceb':'#edf4ff';"
"}"

"async function checkedFetch(url,options={}){"
"const response=await fetch(url,options);"

"if(response.status===401){"
"window.location='/login';"
"throw new Error('Session expired');"
"}"

"if(!response.ok){"
"const text=await response.text();"
"throw new Error(text||('HTTP '+response.status));"
"}"

"return response;"
"}"

"async function loadStatus(){"
"try{"
"const response=await checkedFetch('/api/status');"
"const data=await response.json();"

"document.getElementById('autoState').textContent="
"data.auto_enabled?'ENABLED':'DISABLED';"

"document.getElementById('durationState').textContent="
"data.ring_duration_seconds+' seconds';"

"document.getElementById('seconds').value="
"data.ring_duration_seconds;"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"async function loadTimetable(){"
"try{"
"const response=await checkedFetch("
"'/api/admin/timetable');"

"const data=await response.json();"
"const body=document.getElementById('timetableBody');"
"body.innerHTML='';"

"document.getElementById('bellCount').textContent="
"data.count+' saved bells';"

"for(const entry of data.entries){"
"const row=document.createElement('tr');"

"const indexCell=document.createElement('td');"
"indexCell.textContent=entry.index+1;"

"const dayCell=document.createElement('td');"
"dayCell.textContent=dayNames[entry.weekday]||"
"('Day '+entry.weekday);"

"const timeCell=document.createElement('td');"
"timeCell.textContent="
"String(entry.hour).padStart(2,'0')+':'+"
"String(entry.minute).padStart(2,'0')+':'+"
"String(entry.second).padStart(2,'0');"

"const actionCell=document.createElement('td');"
"const deleteButton=document.createElement('button');"
"deleteButton.type='button';"
"deleteButton.className='danger';"
"deleteButton.textContent='Delete';"
"deleteButton.addEventListener("
"'click',()=>deleteBell(entry.index));"

"actionCell.appendChild(deleteButton);"
"row.appendChild(indexCell);"
"row.appendChild(dayCell);"
"row.appendChild(timeCell);"
"row.appendChild(actionCell);"
"body.appendChild(row);"
"}"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"document.getElementById('addBellForm')"
".addEventListener('submit',async event=>{"
"event.preventDefault();"

"const parameters=new URLSearchParams({"
"weekday:document.getElementById('weekday').value,"
"hour:document.getElementById('hour').value,"
"minute:document.getElementById('minute').value"
"});"

"try{"
"await checkedFetch("
"'/api/admin/timetable/add',"
"{"
"method:'POST',"
"headers:{"
"'Content-Type':'application/x-www-form-urlencoded'"
"},"
"body:parameters.toString()"
"});"

"showMessage('Bell added and saved.');"
"await loadTimetable();"
"await loadStatus();"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"});"

"async function deleteBell(index){"
"if(!confirm('Delete this timetable bell?'))return;"

"try{"
"await checkedFetch("
"'/api/admin/timetable/delete',"
"{"
"method:'POST',"
"headers:{"
"'Content-Type':'application/x-www-form-urlencoded'"
"},"
"body:'index='+encodeURIComponent(index)"
"});"

"showMessage('Bell deleted and timetable saved.');"
"await loadTimetable();"
"await loadStatus();"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"async function restoreDefaults(){"
"if(!confirm("
"'Replace the current timetable with 75 default bells?'"
"))return;"

"try{"
"await checkedFetch("
"'/api/admin/timetable/defaults',"
"{method:'POST'});"

"showMessage('Default timetable restored.');"
"await loadTimetable();"
"await loadStatus();"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"function formatEventTime(event){"
"if(!event.year){"
"return 'Timestamp unavailable';"
"}"

"return "
"String(event.year).padStart(4,'0')+'-'+"
"String(event.month).padStart(2,'0')+'-'+"
"String(event.date).padStart(2,'0')+' '+"
"String(event.hour).padStart(2,'0')+':'+"
"String(event.minute).padStart(2,'0')+':'+"
"String(event.second).padStart(2,'0');"
"}"

"async function loadEvents(){"
"try{"
"const response=await checkedFetch("
"'/api/admin/events');"

"const data=await response.json();"

"document.getElementById('eventCount').textContent="
"data.count+' stored events';"

"const body=document.getElementById('eventBody');"
"body.innerHTML='';"

"if(data.events.length===0){"
"const row=document.createElement('tr');"
"const cell=document.createElement('td');"
"cell.colSpan=4;"
"cell.textContent='No event records';"
"row.appendChild(cell);"
"body.appendChild(row);"
"return;"
"}"

"for(const event of data.events){"
"const row=document.createElement('tr');"

"const sequenceCell=document.createElement('td');"
"sequenceCell.textContent=event.sequence;"

"const timeCell=document.createElement('td');"
"timeCell.textContent=formatEventTime(event);"

"const nameCell=document.createElement('td');"
"nameCell.textContent=event.name;"

"const valueCell=document.createElement('td');"
"valueCell.textContent=event.value;"

"row.appendChild(sequenceCell);"
"row.appendChild(timeCell);"
"row.appendChild(nameCell);"
"row.appendChild(valueCell);"

"body.appendChild(row);"
"}"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"async function clearEvents(){"
"if(!confirm("
"'Clear all stored event records?'"
"))return;"

"try{"
"await checkedFetch("
"'/api/admin/events/clear',"
"{method:'POST'});"

"showMessage("
"'Event log cleared. An audit record was retained.'"
");"

"await loadEvents();"
"await loadStatus();"
"}catch(error){"
"showMessage(error.message,true);"
"}"
"}"

"loadStatus();"
"loadTimetable();"
"loadEvents();"
"</script>"

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
 * Configuration form helpers
 * ========================================================= */

static bool read_single_form_value(
    httpd_req_t *request,
    const char *field_name,
    char *value,
    size_t value_size
)
{
    if (
        request == NULL ||
        field_name == NULL ||
        value == NULL ||
        value_size == 0U
    )
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

    body[received_total] = '\0';

    size_t field_length =
        strlen(field_name);

    if (
        strncmp(
            body,
            field_name,
            field_length
        ) != 0 ||
        body[field_length] != '='
    )
    {
        return false;
    }

    const char *form_value =
        body + field_length + 1U;

    size_t form_value_length =
        strlen(form_value);

    if (
        form_value_length == 0U ||
        form_value_length >=
            value_size
    )
    {
        return false;
    }

    memcpy(
        value,
        form_value,
        form_value_length + 1U
    );

    return true;
}

static bool parse_unsigned_decimal(
    const char *text,
    uint32_t *result
)
{
    if (
        text == NULL ||
        result == NULL ||
        text[0] == '\0'
    )
    {
        return false;
    }

    uint32_t value = 0;

    for (size_t i = 0;
         text[i] != '\0';
         i++)
    {
        if (
            text[i] < '0' ||
            text[i] > '9'
        )
        {
            return false;
        }

        uint32_t digit =
            (uint32_t)(
                text[i] - '0'
            );

        if (value > 1000U)
        {
            return false;
        }

        value =
            value * 10U +
            digit;
    }

    *result = value;

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

    snprintf(
        status.announcement_state,
        sizeof(status.announcement_state),
        "UNAVAILABLE"
    );

    if (current_status_provider != NULL)
    {
        current_status_provider(
            &status
        );
    }

    char json[2200];

    int written =
        snprintf(
            json,
            sizeof(json),

            "{"
            "\"auto_enabled\":%s,"
            "\"alarm_active\":%s,"
            "\"rtc_ready\":%s,"
            "\"wifi_connected\":%s,"
            "\"mqtt_connected\":%s,"
            "\"nvs_ready\":%s,"
            "\"announcement_pa_active\":%s,"
            "\"bell_blocked_by_announcement\":%s,"
            "\"timetable_count\":%lu,"
            "\"event_log_count\":%lu,"
            "\"ring_duration_seconds\":%lu,"
            "\"uptime_seconds\":%lu,"
            "\"nvs_total_entries\":%lu,"
            "\"nvs_used_entries\":%lu,"
            "\"nvs_free_entries\":%lu,"
            "\"app_partition_size_bytes\":%lu,"
            "\"timetable_capacity\":%lu,"
            "\"event_log_capacity\":%lu,"
            "\"mqtt_disconnect_count\":%lu,"
            "\"announcement_state\":\"%s\","
            "\"current_time\":\"%s\","
            "\"next_bell\":\"%s\","
            "\"ip_address\":\"%s\","
            "\"mqtt_topic\":\"%s\","
            "\"mqtt_broker\":\"%s\","
            "\"mqtt_diagnostic\":\"%s\","
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

            status.mqtt_connected
                ? "true"
                : "false",

            status.nvs_ready
                ? "true"
                : "false",

            status.announcement_pa_active
                ? "true"
                : "false",

            status.bell_blocked_by_announcement
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

            (unsigned long)
                status.nvs_total_entries,

            (unsigned long)
                status.nvs_used_entries,

            (unsigned long)
                status.nvs_free_entries,

            (unsigned long)
                status.app_partition_size_bytes,

            (unsigned long)
                status.timetable_capacity,

            (unsigned long)
                status.event_log_capacity,

            (unsigned long)
                status.mqtt_disconnect_count,

            status.announcement_state,
            status.current_time,
            status.next_bell,
            status.ip_address,
            status.mqtt_topic,
            status.mqtt_broker,
            status.mqtt_diagnostic,
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
 * Authenticated alarm-control handlers
 * ========================================================= */

static esp_err_t admin_auto_handler(
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

    char value[4];

    if (!read_single_form_value(
            request,
            "enabled",
            value,
            sizeof(value)
        ))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid AUTO setting"
        );
    }

    bool enabled;

    if (strcmp(value, "1") == 0)
    {
        enabled = true;
    }
    else if (strcmp(value, "0") == 0)
    {
        enabled = false;
    }
    else
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "AUTO value must be 0 or 1"
        );
    }

    if (
        current_control_handlers
            .set_auto_enabled == NULL
    )
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "AUTO control unavailable"
        );
    }

    esp_err_t result =
        current_control_handlers
            .set_auto_enabled(
                enabled
            );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Web AUTO update failed: %s",
            esp_err_to_name(result)
        );

        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to save AUTO setting"
        );
    }

    return redirect_to(
        request,
        "/admin"
    );
}

static esp_err_t admin_duration_handler(
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

    char value[12];

    if (!read_single_form_value(
            request,
            "seconds",
            value,
            sizeof(value)
        ))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid duration"
        );
    }

    uint32_t seconds = 0;

    if (
        !parse_unsigned_decimal(
            value,
            &seconds
        ) ||
        seconds < 1U ||
        seconds > 60U
    )
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Duration must be 1 to 60 seconds"
        );
    }

    if (
        current_control_handlers
            .set_ring_duration == NULL
    )
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Duration control unavailable"
        );
    }

    esp_err_t result =
        current_control_handlers
            .set_ring_duration(
                seconds
            );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Web duration update failed: %s",
            esp_err_to_name(result)
        );

        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to save duration"
        );
    }

    return redirect_to(
        request,
        "/admin"
    );
}


/* =========================================================
 * Timetable HTTP helpers
 * ========================================================= */

static esp_err_t send_plain_status(
    httpd_req_t *request,
    const char *status,
    const char *message
)
{
    httpd_resp_set_status(
        request,
        status
    );

    httpd_resp_set_type(
        request,
        "text/plain"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        message,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t send_no_content(
    httpd_req_t *request
)
{
    httpd_resp_set_status(
        request,
        "204 No Content"
    );

    return httpd_resp_send(
        request,
        NULL,
        0
    );
}

static bool read_admin_form_body(
    httpd_req_t *request,
    char *body,
    size_t body_size
)
{
    if (
        request == NULL ||
        body == NULL ||
        body_size < 2U
    )
    {
        return false;
    }

    if (
        request->content_len == 0U ||
        request->content_len >= body_size
    )
    {
        return false;
    }

    size_t received_total = 0U;

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

    body[received_total] = '\0';

    return true;
}

static bool extract_form_field(
    const char *body,
    const char *field_name,
    char *value,
    size_t value_size
)
{
    if (
        body == NULL ||
        field_name == NULL ||
        value == NULL ||
        value_size == 0U
    )
    {
        return false;
    }

    size_t field_name_length =
        strlen(field_name);

    const char *cursor = body;

    while (*cursor != '\0')
    {
        const char *segment_end =
            strchr(
                cursor,
                '&'
            );

        if (segment_end == NULL)
        {
            segment_end =
                cursor +
                strlen(cursor);
        }

        const char *equals =
            memchr(
                cursor,
                '=',
                (size_t)(
                    segment_end -
                    cursor
                )
            );

        if (equals != NULL)
        {
            size_t name_length =
                (size_t)(
                    equals -
                    cursor
                );

            if (
                name_length ==
                    field_name_length &&
                strncmp(
                    cursor,
                    field_name,
                    field_name_length
                ) == 0
            )
            {
                const char *field_value =
                    equals + 1;

                size_t field_value_length =
                    (size_t)(
                        segment_end -
                        field_value
                    );

                if (
                    field_value_length == 0U ||
                    field_value_length >=
                        value_size
                )
                {
                    return false;
                }

                memcpy(
                    value,
                    field_value,
                    field_value_length
                );

                value[
                    field_value_length
                ] = '\0';

                return true;
            }
        }

        if (*segment_end == '\0')
        {
            break;
        }

        cursor =
            segment_end + 1;
    }

    return false;
}

static bool parse_form_number(
    const char *body,
    const char *field_name,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t *result
)
{
    char value[16];

    if (!extract_form_field(
            body,
            field_name,
            value,
            sizeof(value)
        ))
    {
        return false;
    }

    uint32_t parsed = 0U;

    if (!parse_unsigned_decimal(
            value,
            &parsed
        ))
    {
        return false;
    }

    if (
        parsed < minimum ||
        parsed > maximum
    )
    {
        return false;
    }

    *result = parsed;

    return true;
}

/* =========================================================
 * Authenticated timetable API
 * ========================================================= */

static esp_err_t admin_timetable_list_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    web_timetable_entry_t entries[
        WEB_TIMETABLE_MAX_ENTRIES
    ];

    size_t count = 0U;

    esp_err_t result =
        web_timetable_bridge_get_all(
            entries,
            WEB_TIMETABLE_MAX_ENTRIES,
            &count
        );

    if (result != ESP_OK)
    {
        return send_plain_status(
            request,
            "500 Internal Server Error",
            "Unable to read timetable"
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

    char chunk[160];

    int written =
        snprintf(
            chunk,
            sizeof(chunk),
            "{\"count\":%u,\"entries\":[",
            (unsigned int)count
        );

    if (
        written < 0 ||
        written >= (int)sizeof(chunk)
    )
    {
        return ESP_FAIL;
    }

    if (
        httpd_resp_send_chunk(
            request,
            chunk,
            written
        ) != ESP_OK
    )
    {
        return ESP_FAIL;
    }

    for (size_t i = 0U;
         i < count;
         i++)
    {
        written =
            snprintf(
                chunk,
                sizeof(chunk),

                "%s{"
                "\"index\":%u,"
                "\"weekday\":%u,"
                "\"hour\":%u,"
                "\"minute\":%u,"
                "\"second\":%u"
                "}",

                i == 0U ? "" : ",",

                (unsigned int)i,

                (unsigned int)
                    entries[i].weekday,

                (unsigned int)
                    entries[i].hour,

                (unsigned int)
                    entries[i].minute,

                (unsigned int)
                    entries[i].second
            );

        if (
            written < 0 ||
            written >= (int)sizeof(chunk)
        )
        {
            return ESP_FAIL;
        }

        if (
            httpd_resp_send_chunk(
                request,
                chunk,
                written
            ) != ESP_OK
        )
        {
            return ESP_FAIL;
        }
    }

    if (
        httpd_resp_send_chunk(
            request,
            "]}",
            2
        ) != ESP_OK
    )
    {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(
        request,
        NULL,
        0
    );
}

static esp_err_t admin_timetable_add_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    char body[
        MAX_ADMIN_FORM_BODY_LENGTH
    ];

    if (!read_admin_form_body(
            request,
            body,
            sizeof(body)
        ))
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Invalid timetable form"
        );
    }

    uint32_t weekday = 0U;
    uint32_t hour = 0U;
    uint32_t minute = 0U;

    if (
        !parse_form_number(
            body,
            "weekday",
            1U,
            7U,
            &weekday
        ) ||
        !parse_form_number(
            body,
            "hour",
            0U,
            23U,
            &hour
        ) ||
        !parse_form_number(
            body,
            "minute",
            0U,
            59U,
            &minute
        )
    )
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Weekday, hour or minute is invalid"
        );
    }

    web_timetable_entry_t entry = {
        .weekday =
            (uint8_t)weekday,

        .hour =
            (uint8_t)hour,

        .minute =
            (uint8_t)minute,

        .second = 0U
    };

    esp_err_t result =
        web_timetable_bridge_add(
            &entry
        );

    if (result == ESP_OK)
    {
        return send_no_content(
            request
        );
    }

    if (result == ESP_ERR_NO_MEM)
    {
        return send_plain_status(
            request,
            "409 Conflict",
            "Timetable is full"
        );
    }

    if (result == ESP_ERR_INVALID_STATE)
    {
        return send_plain_status(
            request,
            "409 Conflict",
            "That bell already exists"
        );
    }

    if (result == ESP_ERR_INVALID_ARG)
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Invalid bell"
        );
    }

    return send_plain_status(
        request,
        "500 Internal Server Error",
        "Failed to save timetable"
    );
}

static esp_err_t admin_timetable_delete_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    char body[
        MAX_ADMIN_FORM_BODY_LENGTH
    ];

    if (!read_admin_form_body(
            request,
            body,
            sizeof(body)
        ))
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Invalid delete request"
        );
    }

    uint32_t index = 0U;

    if (!parse_form_number(
            body,
            "index",
            0U,
            WEB_TIMETABLE_MAX_ENTRIES - 1U,
            &index
        ))
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Invalid timetable index"
        );
    }

    esp_err_t result =
        web_timetable_bridge_delete(
            (size_t)index
        );

    if (result == ESP_OK)
    {
        return send_no_content(
            request
        );
    }

    if (result == ESP_ERR_INVALID_STATE)
    {
        return send_plain_status(
            request,
            "409 Conflict",
            "At least one bell must remain"
        );
    }

    if (result == ESP_ERR_INVALID_ARG)
    {
        return send_plain_status(
            request,
            "400 Bad Request",
            "Bell no longer exists"
        );
    }

    return send_plain_status(
        request,
        "500 Internal Server Error",
        "Failed to delete bell"
    );
}

static esp_err_t admin_timetable_defaults_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    esp_err_t result =
        web_timetable_bridge_restore_defaults();

    if (result != ESP_OK)
    {
        return send_plain_status(
            request,
            "500 Internal Server Error",
            "Failed to restore defaults"
        );
    }

    return send_no_content(
        request
    );
}

/* =========================================================
 * Authenticated event-log API
 * ========================================================= */

static esp_err_t admin_events_list_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    event_log_record_t records[
        WEB_EVENT_MAX_RECORDS
    ];

    size_t count = 0U;

    esp_err_t result =
        web_event_bridge_get_all(
            records,
            WEB_EVENT_MAX_RECORDS,
            &count
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Unable to read event log: %s",
            esp_err_to_name(result)
        );

        return send_plain_status(
            request,
            "500 Internal Server Error",
            "Unable to read event log"
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

    char chunk[320];

    int written =
        snprintf(
            chunk,
            sizeof(chunk),
            "{\"count\":%u,\"events\":[",
            (unsigned int)count
        );

    if (
        written < 0 ||
        written >= (int)sizeof(chunk)
    )
    {
        return ESP_FAIL;
    }

    if (
        httpd_resp_send_chunk(
            request,
            chunk,
            written
        ) != ESP_OK
    )
    {
        return ESP_FAIL;
    }

    for (size_t i = 0U;
         i < count;
         i++)
    {
        const event_log_record_t *record =
            &records[i];

        const char *event_name =
            event_log_type_name(
                record->type
            );

        written =
            snprintf(
                chunk,
                sizeof(chunk),

                "%s{"
                "\"sequence\":%lu,"
                "\"type\":%u,"
                "\"name\":\"%s\","
                "\"value\":%ld,"
                "\"year\":%u,"
                "\"month\":%u,"
                "\"date\":%u,"
                "\"hour\":%u,"
                "\"minute\":%u,"
                "\"second\":%u"
                "}",

                i == 0U ? "" : ",",

                (unsigned long)
                    record->sequence,

                (unsigned int)
                    record->type,

                event_name,

                (long)
                    record->value,

                (unsigned int)
                    record->time.year,

                (unsigned int)
                    record->time.month,

                (unsigned int)
                    record->time.date,

                (unsigned int)
                    record->time.hour,

                (unsigned int)
                    record->time.minute,

                (unsigned int)
                    record->time.second
            );

        if (
            written < 0 ||
            written >= (int)sizeof(chunk)
        )
        {
            return ESP_FAIL;
        }

        if (
            httpd_resp_send_chunk(
                request,
                chunk,
                written
            ) != ESP_OK
        )
        {
            return ESP_FAIL;
        }
    }

    if (
        httpd_resp_send_chunk(
            request,
            "]}",
            2
        ) != ESP_OK
    )
    {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(
        request,
        NULL,
        0
    );
}

static esp_err_t admin_events_clear_handler(
    httpd_req_t *request
)
{
    if (!request_is_authenticated(
            request
        ))
    {
        return send_plain_status(
            request,
            "401 Unauthorized",
            "Authentication required"
        );
    }

    esp_err_t result =
        web_event_bridge_clear();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Unable to clear event log: %s",
            esp_err_to_name(result)
        );

        return send_plain_status(
            request,
            "500 Internal Server Error",
            "Unable to clear event log"
        );
    }

    return send_no_content(
        request
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
    web_status_provider_t status_provider,
    const web_control_handlers_t *control_handlers
)
{
    if (server_handle != NULL)
    {
        return ESP_OK;
    }

    if (
        status_provider == NULL ||
        control_handlers == NULL ||
        control_handlers->set_auto_enabled == NULL ||
        control_handlers->set_ring_duration == NULL
    )
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

    current_control_handlers =
        *control_handlers;

    httpd_config_t configuration =
        HTTPD_DEFAULT_CONFIG();

    configuration.server_port = 80;
    configuration.max_uri_handlers = 20;
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
        },
        {
            .uri = "/api/admin/auto",
            .method = HTTP_POST,
            .handler = admin_auto_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/duration",
            .method = HTTP_POST,
            .handler = admin_duration_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/timetable",
            .method = HTTP_GET,
            .handler = admin_timetable_list_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/timetable/add",
            .method = HTTP_POST,
            .handler = admin_timetable_add_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/timetable/delete",
            .method = HTTP_POST,
            .handler = admin_timetable_delete_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/timetable/defaults",
            .method = HTTP_POST,
            .handler = admin_timetable_defaults_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/events",
            .method = HTTP_GET,
            .handler = admin_events_list_handler,
            .user_ctx = NULL
        },
        {
            .uri = "/api/admin/events/clear",
            .method = HTTP_POST,
            .handler = admin_events_clear_handler,
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

    memset(
        &current_control_handlers,
        0,
        sizeof(current_control_handlers)
    );

    return result;
}

bool web_server_is_running(void)
{
    return server_handle != NULL;
}

