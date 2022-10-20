#ifndef NET_EVENTSOURCE_H
#define NET_EVENTSOURCE_H

#include "esp_system.h"

/**
 * Implementation of a TCP server for HTML5 Server-Sent-Events (EventSource in JavaScript)
 */

typedef esp_err_t (*eventsource_joined_cb_t) (int session);

void eventsource_init(void);
void eventsource_start(void);
void eventsource_stop(void);
void eventsource_destroy(void);

void eventsource_set_joined_cb(eventsource_joined_cb_t cb);

esp_err_t eventsource_send_eventstr(int session, int id, const char* event, const char* data);
esp_err_t eventsource_sendall_eventstr(int id, const char* event, const char* data);

#endif
