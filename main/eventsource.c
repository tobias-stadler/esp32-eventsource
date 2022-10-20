#include "eventsource.h"

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "errno.h"

#include "defutil.h"

#define EVENTSOURCE_RXSIZE 4096
#define EVENTSOURCE_TXSIZE 1024
#define EVENTSOURCE_MAXCON 5
#define EVENTSOURCE_PORT 8080

#define EVENTSOURCE_ENDPOINT "GET /api.sse"

static const char* TAG = "NET/EventSource";

//\r\nTransfer-Encoding: chunked  retry:5000\n
static const char* resp_accept = "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: text/event-stream\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: *\r\n\r\n\r\n";

static int conns[EVENTSOURCE_MAXCON];
static int listen_sock = -1;

static char* rx_buf = NULL;
static char* tx_buf = NULL;

static bool running = false;

static SemaphoreHandle_t x_mutex = NULL;

static eventsource_joined_cb_t joined_cb = NULL;

static esp_err_t sess_write(int i, const char* buf, size_t len);

static void sess_recv(int i, size_t len)
{
    if(i>=EVENTSOURCE_MAXCON) return;
    if(len < strlen(EVENTSOURCE_ENDPOINT)) return;
    if(STARTS_WITH(rx_buf, EVENTSOURCE_ENDPOINT))
    {
        sess_write(i, resp_accept, strlen(resp_accept));
        //Invoke join callback after client was accepted
        if(joined_cb != NULL) joined_cb(i);
    }
}

static int sess_available(void)
{
    for(uint8_t i = 0; i < EVENTSOURCE_MAXCON; i++)
    {
        if(conns[i] == -1) return i;
    }
    return -1;
}

static int sess_accept(void)
{
    int i = sess_available();
    if (i < 0)
    {
        return -1;
    }

    int fd = accept(listen_sock, NULL, NULL);
    if(fd < 0)
    {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
        return -1;
    }
    conns[i] = fd;
    ESP_LOGI(TAG, "Opened session %d", i);
    return i;
}

static void sess_close(int i)
{
    if(i>=EVENTSOURCE_MAXCON) return;
    int fd = conns[i];
    if(fd>0) close(fd);
    conns[i] = -1;
    ESP_LOGI(TAG, "Closed session %d", i);
}

static esp_err_t sess_write(int i, const char* buf, size_t len)
{
    if(i>=EVENTSOURCE_MAXCON) return ESP_FAIL;
    int fd = conns[i];
    if(fd < 0) return ESP_OK;
    if(write(fd, buf, len) < 0)
    {
        ESP_LOGE(TAG, "Failed writing to socket. Closing...");
        sess_close(i);
        return ESP_FAIL;
    }
    return ESP_OK;
}

//Task running all TCP networking
static void eventsource_task(void* param)
{
    memset(conns, -1, EVENTSOURCE_MAXCON*sizeof(int));

    ESP_LOGI(TAG, "Starting HTML5 EventSource...");

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        goto fail;
    }
    ESP_LOGI(TAG,"Socket created");
    int err = 0;

    int en_int = 1;
    err = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &en_int, sizeof(en_int));
    if(err)
    {
        ESP_LOGE(TAG, "Failed to set REUSEADDR");
        goto fail;
    }
    ESP_LOGI(TAG,"Configured socket");

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(EVENTSOURCE_PORT);

    err = bind(listen_sock, (struct sockaddr*)&dest_addr, sizeof(struct sockaddr));
    if(err)
    {
        ESP_LOGE(TAG, "Failed to bind socket");
        goto fail;
    }
    ESP_LOGI(TAG, "Bound to PORT %d", EVENTSOURCE_PORT);
    err = listen(listen_sock, 1);
    if(err)
    {
        ESP_LOGE(TAG, "Failed to start listening for connections!");
        goto fail;
    }
    ESP_LOGI(TAG, "Listening for connections...");

    fd_set in_set;
    int max_fd;

    while(running)
    {
        FD_ZERO(&in_set);
        FD_SET(listen_sock, &in_set);
        max_fd = listen_sock;

        for(uint8_t i = 0; i < EVENTSOURCE_MAXCON; i++)
        {
            int fd = conns[i];
            if(fd > max_fd) max_fd = fd;
            if(fd > 0) FD_SET(fd, &in_set);
        }

        int active = select(max_fd + 1, &in_set, NULL, NULL, NULL);
        ESP_LOGD(TAG, "Task woke up");
        if(active > 0) {
            //New connection requested
            if(FD_ISSET(listen_sock, &in_set)) {
                int new_sess = sess_accept();
                if(new_sess < 0) {
                    ESP_LOGE(TAG, "Failed to accept connection!");
                }
            }

            //Process received data
            for(uint8_t i = 0; i < EVENTSOURCE_MAXCON; i++)
            {
                int fd = conns[i];
                if(fd > 0 && FD_ISSET(fd, &in_set)) {

                    size_t chunksize = read(fd, rx_buf, EVENTSOURCE_RXSIZE);
                    if(chunksize){
                        sess_recv(i, chunksize);
                    }
                }
            }
        }
    }

    fail:
    running = false;
    ESP_LOGI(TAG, "Stopped HTML5 EventSource");
    vTaskDelete(NULL);
}

static esp_err_t prepare_output_eventstr(int id, const char* event, const char* data)
{
    xSemaphoreTake(x_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_OK;

    static const char* id_header = "id: ";
    static const char* event_header = "event: ";
    static const char* data_header = "data: ";

    size_t event_header_len = strlen(event_header);
    size_t data_header_len = strlen(data_header);

    size_t total_len = 0;
    //Ensure first char in TX Buffer is a null terminator for strcat to work
    tx_buf[0] = 0;

    if(id >= 0)
    {
        strcpy(tx_buf, id_header);
        sprintf(tx_buf + strlen(id_header), "%d", id);
        strcat(tx_buf, "\n");
        total_len += strlen(tx_buf);
    }

    if(event != NULL)
    {
        total_len += strlen(event) + 1;
        total_len += event_header_len;
    }

    if(data != NULL)
    {
        total_len += strlen(data) + 1;
        total_len += data_header_len;
    }

    if(total_len == 0)
    {
        ESP_LOGE(TAG, "Couldn't send event as input was empty!");
        ret = ESP_FAIL;
        goto fail;
    }

    if((total_len + 2) > EVENTSOURCE_TXSIZE)
    {
        ESP_LOGE(TAG, "Failed to send event! Input too large! Increase EVENTSOURCE_TXSIZE");
        ret = ESP_FAIL;
        goto fail;
    }

    if(event != NULL)
    {
        strcat(tx_buf, "event: ");
        strcat(tx_buf, event);
        strcat(tx_buf, "\n");
    }

    if(data != NULL)
    {
        strcat(tx_buf, "data: ");
        strcat(tx_buf, data);
        strcat(tx_buf, "\n");
    }
    strcat(tx_buf, "\n");



    fail:
    xSemaphoreGive(x_mutex);
    return ret;
}

/**
 * Sends an event to session with session id @param session
 * @param id (use -1 to not send id header)
 * @param event (use NULL to not send event header)
 * @param data (use NULL to not send line of data)
 */
esp_err_t eventsource_send_eventstr(int session, int id, const char* event, const char* data)
{
    if(prepare_output_eventstr(id, event, data) != ESP_OK) return ESP_FAIL;
    return sess_write(session, tx_buf, strlen(tx_buf));
}


/**
 * Sends an event to all sessions
 * See @link #eventsource_send_eventstr
 */
esp_err_t eventsource_sendall_eventstr(int id, const char* event, const char* data)
{
    if(prepare_output_eventstr(id, event, data) != ESP_OK) return ESP_FAIL;
    for(uint8_t i = 0; i<EVENTSOURCE_MAXCON; i++)
    {
        sess_write(i, tx_buf, strlen(tx_buf));
    }
    return ESP_OK;
}

/**
 * Sets callback which gets notified after a client gets accepted
 */
void eventsource_set_joined_cb(eventsource_joined_cb_t cb)
{
    joined_cb = cb;
}

void eventsource_init(void)
{
    if(rx_buf == NULL) rx_buf = (char*)calloc(EVENTSOURCE_RXSIZE, sizeof(char));
    if(tx_buf == NULL) tx_buf = (char*)calloc(EVENTSOURCE_TXSIZE, sizeof(char));
    if(x_mutex == NULL) x_mutex = xSemaphoreCreateMutex();
}

void eventsource_start(void)
{
    if(running) {
        ESP_LOGE(TAG, "Failed to start EventSource as it is already running!");
        return;
    }

    running = true;


    xTaskCreate(eventsource_task, "eventsource", 4096, NULL, 5, NULL);
}

void eventsource_stop(void)
{
    if(!running) {
        ESP_LOGE(TAG, "Failed to stop EventSource as it is not running!");
        return;
    }

    running = false;

}

void eventsource_destroy(void)
{
    if(running) {
        ESP_LOGE(TAG, "Failed to destroy EventSource as it is still running!");
        return;
    }

    if(rx_buf != NULL) free(rx_buf);
    if(tx_buf != NULL) free(tx_buf);
    if(x_mutex != NULL) vSemaphoreDelete(x_mutex);
    rx_buf = NULL;
    tx_buf = NULL;
    x_mutex = NULL;
}

