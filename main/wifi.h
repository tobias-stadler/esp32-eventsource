#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <esp_system.h>

/**
 *
 * Wrapper for ESP32 WiFi functionality
 * SSIDs and passwords are configurable using "idf.py menuconfig"
 *
 */

void network_wifi_init(void);
void network_wifi_start(void);
void network_wifi_stop(void);

void network_wifi_sta_init(void);
void network_wifi_ap_init(void);
void network_wifi_sta_ap_init(void);

bool network_wifi_wait_connected(void);

#endif
