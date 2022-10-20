#include "webserver.h"

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "tcpip_adapter.h"
#include <sys/socket.h>

#include <esp_http_server.h>

#include "defutil.h"

#define WEBSERVER_MAX_PATH_SIZE (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define WEBSERVER_TEMP_BUFSIZE  4096

//Currently only ASCII chars supported! wchar_t breaks memchr :(
#define WEBSERVER_TEMPLATE_PLACEHOLDER '$'

#define WEBSERVER_API_SUBSTRING "/api/"
#define WEBSERVER_API_ENDPOINT "/api/*"

static const char* TAG = "NET/WEBSERVER";

typedef struct {
    char temp_buf[WEBSERVER_TEMP_BUFSIZE];
} server_data_t;


static server_data_t* server_data = NULL;
static httpd_handle_t http_server;

static web_template_cb_t template_cb = NULL;
static web_api_cb_t api_cb = NULL;

static const char* BASE_PATH = "/spiffs";

static esp_err_t redirect_to_index(httpd_req_t* req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req,"Location", "/index.html");
    ESP_LOGI(TAG, "Redirected to index!");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t set_content_type_from_file(httpd_req_t* req, const char* filename)
{
    if(ENDS_WITH(filename, ".html"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if(ENDS_WITH(filename, ".js"))
    {
        return httpd_resp_set_type(req, "text/javascript");
    }
    else if(ENDS_WITH(filename, ".css"))
    {
        return httpd_resp_set_type(req, "text/css");
    }
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t http_send_file_templated(httpd_req_t* req, const char* filename)
{
    FILE* fd = fopen(filename, "r");
    if(fd == 0)
    {
        httpd_resp_send_500(req);
        ESP_LOGE(TAG, "Failed to open templated file: %s", filename);
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filename);

    size_t chunksize;
    char* chunk = ((server_data_t*)(req->user_ctx))->temp_buf;
    char* chunk_after;

    uint8_t template_index = 0;

    while(true)
    {
        chunksize = fread(chunk, 1, (WEBSERVER_TEMP_BUFSIZE), fd);
        if(!chunksize) break;

        //Search for placeholders and split the chunks at the placeholder positions.
        //After splitting the chunk control is handed to the user defined callback function

        chunk_after = chunk + chunksize;
        size_t len_left = chunksize;
        char* part = chunk;
        size_t part_len;
        char* temp_indicator = NULL;

        do {
            temp_indicator = memchr(part, WEBSERVER_TEMPLATE_PLACEHOLDER, len_left);
            if(temp_indicator) {
                (*temp_indicator) = 0;
                part_len = temp_indicator-part;
                if(part_len)
                {
                    if(httpd_resp_send_chunk(req, part, part_len) != ESP_OK) goto fail;
                }
                if(template_cb(req, filename, template_index) != ESP_OK) goto fail;
                template_index++;
                part = temp_indicator + 1;
                len_left = chunk_after-part;
            }
        } while (temp_indicator && part < chunk_after);

        if(len_left)
        {
            if(httpd_resp_send_chunk(req, part, len_left) != ESP_OK) goto fail;
        }
    }

    fclose(fd);
    ESP_LOGI(TAG, "Sent templated file: %s", filename);
    return httpd_resp_send_chunk(req, NULL, 0);

    fail:
    fclose(fd);
    ESP_LOGE(TAG, "Failed to send templated file: %s", filename);
    httpd_resp_send_chunk(req, NULL, 0);
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

static esp_err_t http_send_file_chunked(httpd_req_t* req, const char* filename)
{
    FILE* fd = fopen(filename, "r");
    if(fd == 0)
    {
        httpd_resp_send_500(req);
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filename);

    size_t chunksize;
    char* chunk = ((server_data_t*)(req->user_ctx))->temp_buf;

    while(true)
    {
        chunksize = fread(chunk, 1, WEBSERVER_TEMP_BUFSIZE,fd);
        if(!chunksize) break;

        if(httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to send file: %s", filename);
            fclose(fd);
            httpd_resp_send_chunk(req, NULL, 0);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

    }
    fclose(fd);
    ESP_LOGI(TAG, "Sent file: %s", filename);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t filename_from_req(httpd_req_t* req, char* filename)
{
    size_t uri_len = strlen(req->uri);
    size_t path_len = uri_len;

    const char* mark = strchr(req->uri,'?');
    const char* hash = strchr(req->uri,'#');

    if(hash) {
        path_len = (hash - req->uri);
    }

    if(mark) {
        path_len = (mark - req->uri);
    }

    if(path_len > uri_len)
    {
        path_len = uri_len;
    }
    if(path_len > WEBSERVER_MAX_PATH_SIZE)
    {
        ESP_LOGE(TAG, "Path out of range!");
        return ESP_FAIL;
    }

    strcpy(filename, BASE_PATH);
    strlcpy(filename + strlen(BASE_PATH), req->uri, path_len + 1);
    //ESP_LOGI(TAG, "Decoded filename: %s", filename);

    struct stat file_stat;
    if(stat(filename, &file_stat) == -1)
    {
        ESP_LOGE(TAG, "File not found: %s", filename);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http_get_handler(httpd_req_t* req)
{
    if(!strcmp(req->uri,"/"))
    {
        return redirect_to_index(req);
    }

    char filename[WEBSERVER_MAX_PATH_SIZE + 1] = {0};

    /*
    //code gets own and remote ip of TCP connection used for the http request
    //not needed (yet)
    int socketfd = httpd_req_to_sockfd(req);

    //local ip
    struct sockaddr_in iface_addr;
    socklen_t iface_socklen = sizeof(iface_addr);
    getsockname(socketfd, (struct sockaddr*)&iface_addr, &iface_socklen);
    //remote ip
    struct sockaddr_in remote_addr;
    socklen_t remote_socklen = sizeof(remote_addr);
    getpeername(socketfd, (struct sockaddr*)&remote_addr, &remote_socklen);
    */

    if(filename_from_req(req, filename) != ESP_OK)
    {
        return httpd_resp_send_404(req);
    }

    //Circumvent cross origin block because the EventSource on PORT 8080 needs to be accessible
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "*");

    if(ENDS_WITH(filename, ".html")){
        return http_send_file_templated(req, filename);
    } else {
        return http_send_file_chunked(req, filename);
    }
}

static esp_err_t http_api_post_handler(httpd_req_t* req)
{
    esp_err_t ret = ESP_OK;

    if(api_cb == NULL)
    {
        httpd_resp_sendstr(req, "API call not handled");
    }
    else
    {
        size_t uri_len = strlen(req->uri);
        size_t substr_len = strlen(WEBSERVER_API_SUBSTRING);
        if(uri_len > substr_len)
        {
            char call_name[(uri_len-substr_len+1)];
            strcpy(call_name, req->uri+substr_len);
            ret = api_cb(req, call_name);
        }
        else
        {
            ret = ESP_FAIL;
        }
    }

    if(ret == ESP_OK)
    {
        httpd_resp_sendstr(req, "API call successful");
    }
    else
    {
        httpd_resp_sendstr(req, "API call failed");
    }
    return ret;
}

static void register_handlers(void) {
    httpd_uri_t get_handler = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = http_get_handler,
            .user_ctx = server_data
    };
    httpd_register_uri_handler(http_server, &get_handler);

    httpd_uri_t post_handler = {
            .uri = WEBSERVER_API_ENDPOINT,
            .method = HTTP_POST,
            .handler = http_api_post_handler,
            .user_ctx = server_data
    };
    httpd_register_uri_handler(http_server, &post_handler);
}

void webserver_init_filesystem(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
            .base_path = BASE_PATH,
            .partition_label = NULL,
            .max_files = 20,
            .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    ESP_ERROR_CHECK(ret);
}

void webserver_init(void)
{
    if(server_data != NULL) return;

    server_data = calloc(1, sizeof(server_data_t));

    webserver_init_filesystem();
}

void webserver_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 5;
    ESP_LOGI(TAG, "Starting HTTP server");

    if(httpd_start(&http_server, &config) == ESP_OK)
    {
        register_handlers();
    }
    else
    {
        ESP_LOGE(TAG, "ERROR occurred while starting HTTP-Server!");
    }
}

void webserver_stop(void)
{
    httpd_stop(http_server);
    ESP_LOGI(TAG, "HTTP server stopped");
}

void webserver_destroy(void)
{
    if(server_data == NULL) return;

    free(server_data);
    server_data = NULL;
}

/**
 * Sets callback which gets notified when a template needs to be processed
 */
void webserver_set_template_cb(web_template_cb_t cb)
{
    template_cb = cb;
}

/**
 * Sets callback which gets notified when an http request reaches the api endpoint
 */
void webserver_set_api_cb(web_api_cb_t cb)
{
    api_cb = cb;
}
