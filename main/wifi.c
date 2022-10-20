#include "wifi.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include <string.h>
#include "mdns.h"

#define NET_WIFI_STA_SSID CONFIG_WIFI_STATION_SSID
#define NET_WIFI_STA_PASSWORD CONFIG_WIFI_STATION_PASSWORD
#define NET_WIFI_AP_SSID CONFIG_WIFI_AP_SSID
#define NET_WIFI_AP_PASSWORD CONFIG_WIFI_AP_PASSWORD
#define NET_WIFI_MAX_RETRY CONFIG_ESP_MAXIMUM_RETRY

#define NET_WIFI_MAX_WAIT_ms 30000

//AP static ip config
#define NET_WIFI_AP_GW PP_HTONL(LWIP_MAKEU32(192,168,138,1))
#define NET_WIFI_AP_MSK PP_HTONL(LWIP_MAKEU32(255,255,255,0))
#define NET_WIFI_AP_IP PP_HTONL(LWIP_MAKEU32(192,168,138,1))

#define NET_AP_HOSTNAME "testboard"

static const char* TAG_STA= "Net/WIFI-STA";
static const char* TAG_AP = "Net/WIFI-AP";

EventGroupHandle_t wifi_event_group;

static wifi_config_t wifi_config_sta;
static wifi_config_t wifi_config_ap;

static int retry_num = 0;

const int WIFI_CONNECTED_BIT = BIT0;

static void event_handler_wifi(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG_AP, "station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG_AP, "station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }

    if(event_id == WIFI_EVENT_STA_START)
    {
        retry_num = 0;
        esp_wifi_connect();
    }
    else if(event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGE(TAG_STA, "Connection FAILED!");
        if(retry_num < NET_WIFI_MAX_RETRY)
        {
            retry_num++;
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGI(TAG_STA, "Trying to reconnect... [%d/%d]", retry_num, CONFIG_ESP_MAXIMUM_RETRY);
        }

    }
}

static void event_handler_ip(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_id == IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_STA, "Received IP: %s", ip4addr_ntoa(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void ap_setup_dhcp_static_host(void)
{
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));

    tcpip_adapter_ip_info_t ip_info;
    ip_info.gw.addr = NET_WIFI_AP_GW;
    ip_info.netmask.addr = NET_WIFI_AP_MSK;
    ip_info.ip.addr = NET_WIFI_AP_IP;
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info));

    dhcps_lease_t lease;
    lease.enable = true;
    lease.start_ip.addr = ip_info.ip.addr + (PP_HTONL(10));
    lease.end_ip.addr = ip_info.ip.addr + (PP_HTONL(20));
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET, TCPIP_ADAPTER_REQUESTED_IP_ADDRESS, &lease, sizeof(dhcps_lease_t)));

    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
}

/*
 *  Currently unused as Windows (contrary to MacOS, Linux) doesn't support mDNS out of the box.
 *  If we want to use non-centralized DNS with Windows we probably have to look into LLMNR.
 *
void network_dns_init(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(NET_AP_HOSTNAME));
}
*/
void network_wifi_init(void)
{
    tcpip_adapter_init();

    wifi_event_group = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler_ip, NULL));

    wifi_config_t config_sta = {
            .sta = {
                    .ssid = NET_WIFI_STA_SSID,
                    .password = NET_WIFI_STA_PASSWORD
            }
    };

    wifi_config_t config_ap = {
            .ap = {
                    .ssid = NET_WIFI_AP_SSID,
                    .password = NET_WIFI_AP_PASSWORD,
                    .ssid_len = strlen(NET_WIFI_AP_SSID),
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK,
                    .ssid_hidden = true,
                    .max_connection = 3
            }
    };

    wifi_config_ap = config_ap;
    wifi_config_sta = config_sta;

}

void network_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_wifi_start());
}

void network_wifi_stop(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
}


void network_wifi_sta_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta));
}

void network_wifi_ap_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ap_setup_dhcp_static_host();
}

void network_wifi_sta_ap_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta));
    ap_setup_dhcp_static_host();
}

/**
 * @return true if STA is successfully connected to WIFI
 */
bool network_wifi_wait_connected(void)
{
    EventBits_t uxBits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, NET_WIFI_MAX_WAIT_ms/portTICK_PERIOD_MS);
    return ((uxBits & WIFI_CONNECTED_BIT) != 0);
}
