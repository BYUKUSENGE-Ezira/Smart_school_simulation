#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    WIFI_MANAGER_NOT_INITIALIZED = 0,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_DISCONNECTED,
    WIFI_MANAGER_ERROR
} wifi_manager_state_t;

/*
 * Initialize Wi-Fi station mode.
 *
 * This function starts the connection process but does not
 * wait for a connection. The alarm remains non-blocking.
 *
 * NVS must already be initialized.
 */
esp_err_t wifi_manager_init(
    const char *ssid,
    const char *password
);

/*
 * Return the current Wi-Fi state.
 */
wifi_manager_state_t wifi_manager_get_state(void);

/*
 * Return true only when the ESP32 has received an IP address.
 */
bool wifi_manager_is_connected(void);

/*
 * Write the current IPv4 address into the supplied buffer.
 *
 * Returns "0.0.0.0" when disconnected.
 */
esp_err_t wifi_manager_get_ip_string(
    char *buffer,
    size_t buffer_size
);

/*
 * Return the number of reconnection attempts since the last
 * successful connection.
 */
uint32_t wifi_manager_reconnect_attempts(void);

/*
 * Return a readable name for a Wi-Fi state.
 */
const char *wifi_manager_state_name(
    wifi_manager_state_t state
);

#endif
