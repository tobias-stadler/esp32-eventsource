#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "webserver.h"
#include "eventsource.h"
#include "esp_log.h"

static const char* TAG = "MAIN";

static volatile bool execution_needed = false;

static esp_err_t webinterface_template_cb(httpd_req_t* req, const char* filename, uint8_t i)
{
    if(!strcmp(filename, "/spiffs/index.html")){
        switch(i)
        {
        case 0:
            httpd_resp_sendstr_chunk(req, "A");
            break;
        case 1:
            httpd_resp_sendstr_chunk(req, "B");
            break;
        case 2:
            httpd_resp_sendstr_chunk(req, "");
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t webinterface_api_cb(httpd_req_t* req, const char* api_call)
{
    //Do not execute heavy loads in this function. Another thread needs to be used as this callback shouldn't do any blocking IO
    //Use queue, etc.

    if(!strcmp(api_call, "execute"))
    {
        ESP_LOGI(TAG,"API: Executing...");
        execution_needed = true;
    }

    return ESP_OK;
}

static esp_err_t webinterface_joined_cb(int session)
{
    eventsource_send_eventstr(session, -1, "reset", "");

    return ESP_OK;
}

void configure_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void configure_network(void)
{
    //Network initialization
    network_wifi_init();
    network_wifi_sta_ap_init();
    network_wifi_start();

    //Webinterface
    webserver_init();
    webserver_set_template_cb(webinterface_template_cb);
    webserver_set_api_cb(webinterface_api_cb);
    webserver_start();

    eventsource_init();
    eventsource_start();
    eventsource_set_joined_cb(webinterface_joined_cb);


    if(network_wifi_wait_connected())
    {
        ESP_LOGI(TAG, "WIFI station connected successfully!");
    }
    else
    {
        ESP_LOGE(TAG, "WIFI station failed to connect!");
    }
}

static void loop_task(void* params)
{
    while(true)
    {
        vTaskDelay(1000/portTICK_PERIOD_MS);
        if(!execution_needed) continue;
            execution_needed = false;

        //Reset client view
        eventsource_sendall_eventstr(-1, "reset", "");
    }
    vTaskDelete(NULL);
}


void app_main(void)
{
    configure_nvs();
    configure_network();

    xTaskCreate(loop_task, "loop", 4096, NULL, 5, NULL);
}
