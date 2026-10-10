#include "mqtt_topics.h"
#include <stdio.h>
#include <string.h>
static char s_topics[5][192];
esp_err_t mqtt_topics_init(const char *base, const char *id) {
    if (!base || !id || !*base || !*id) return ESP_ERR_INVALID_ARG;
    const char *names[] = {"status","event","command","config/set","config/ack"};
    for (int i=0;i<5;i++) snprintf(s_topics[i], sizeof(s_topics[i]), "%s/device/%s/%s", base,id,names[i]);
    return ESP_OK;
}
const char *mqtt_topic_status(void){return s_topics[0];}
const char *mqtt_topic_event(void){return s_topics[1];}
const char *mqtt_topic_command(void){return s_topics[2];}
const char *mqtt_topic_config_set(void){return s_topics[3];}
const char *mqtt_topic_config_ack(void){return s_topics[4];}
