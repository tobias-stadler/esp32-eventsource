#ifndef NET_WEBSERVER_H
#define NET_WEBSERVER_H

#include <esp_http_server.h>

/**
 *
 * Simple SPIFFS based http server implementation capable of simple templating of html files
 *
 */

typedef esp_err_t (*web_template_cb_t) (httpd_req_t* req, const char* filename, uint8_t i);
typedef esp_err_t (*web_api_cb_t) (httpd_req_t* req, const char* api_call);

void webserver_init(void);
void webserver_start(void);
void webserver_stop(void);
void webserver_destroy(void);

void webserver_set_template_cb(web_template_cb_t cb);
void webserver_set_api_cb(web_api_cb_t cb);

#endif
