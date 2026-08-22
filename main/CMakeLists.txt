idf_component_register(
    SRCS
        "main.c"
        "event_log.c"
        "schedule_guard.c"
        "access_control.c"
        "system_watchdog.c"
        "wifi_manager.c"
        "web_server.c"
        "web_auth.c"
        "web_timetable_bridge.c"
        "web_event_bridge.c"
        "mqtt_manager.c"
        "announcement_manager.c"
        "announcement_controls.c"
        "announcement_output.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        esp_system
        app_update
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        mqtt
)





