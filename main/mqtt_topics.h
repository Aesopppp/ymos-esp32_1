#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H
#include <stddef.h>
#include "esp_err.h"
esp_err_t mqtt_topics_init(const char *base, const char *device_id);
const char *mqtt_topic_status(void);
const char *mqtt_topic_event(void);
const char *mqtt_topic_command(void);
const char *mqtt_topic_config_set(void);
const char *mqtt_topic_config_ack(void);
#endif
