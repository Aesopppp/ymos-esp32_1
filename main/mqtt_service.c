#include "mqtt_service.h"
#include "cfg_service.h"
#include "mqtt_topics.h"
#include "board_config.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_event.h"
#include <string.h>
static const char *TAG="mqtt_service";
static esp_mqtt_client_handle_t s_client; static event_bus_t s_bus; static bool s_connected;
static char s_device[64], s_client_id[64];
static char s_command[512];
static void publish_status(const char *state) { if (!s_connected) return; char b[256]; snprintf(b,sizeof(b),"{\"version\":1,\"type\":\"status\",\"payload\":{\"state\":\"%s\"}}",state); esp_mqtt_client_publish(s_client,mqtt_topic_status(),b,0,1,1); }
static void publish_hello(void) { char b[512]; snprintf(b,sizeof(b),"{\"version\":1,\"type\":\"hello\",\"transport\":\"acos_websocket\",\"device_id\":\"%s\",\"features\":{\"wake_and_speak\":true,\"scheduled_reminder\":true},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"upload_frame_ms\":20,\"download_frame_ms\":60}}",s_device); esp_mqtt_client_publish(s_client,mqtt_topic_event(),b,0,1,0); }
static void on_event(void *a, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t e=data;
    if (id==MQTT_EVENT_CONNECTED) { s_connected=true; publish_status("online"); publish_hello(); esp_mqtt_client_subscribe(s_client,mqtt_topic_command(),1); esp_mqtt_client_subscribe(s_client,mqtt_topic_config_set(),1); event_bus_publish(EV_MQTT_CONNECTED,NULL,0); }
    else if (id==MQTT_EVENT_DISCONNECTED) { s_connected=false; event_bus_publish(EV_MQTT_DISCONNECTED,NULL,0); }
    else if (id==MQTT_EVENT_ERROR) event_bus_publish(EV_MQTT_ERROR,NULL,0);
    else if (id==MQTT_EVENT_DATA && e->topic_len > 0) {
        if ((size_t)e->topic_len == strlen(mqtt_topic_command()) && strncmp(e->topic,mqtt_topic_command(),e->topic_len)==0) {
            size_t n = e->data_len < sizeof(s_command)-1 ? e->data_len : sizeof(s_command)-1;
            memcpy(s_command, e->data, n); s_command[n] = 0;
            event_bus_publish(EV_MQTT_COMMAND, s_command, n + 1);
        }
    }
}
esp_err_t mqtt_service_init(event_bus_t bus) { s_bus=bus; return ESP_OK; }
esp_err_t mqtt_service_start(void) {
    if (s_client) return ESP_OK; /* 包括正在连接，重复 IP 事件不得泄漏客户端。 */
    char host[128], user[96], pass[128], base[96]; int port=1883; bool enabled=cfg_service_get_bool(CFG_KEY_MQTT_ENABLED,true);
    if(!enabled) return ESP_ERR_INVALID_STATE;
    if(cfg_service_get_identity((char[20]){0},20,s_device,sizeof(s_device),s_client_id,sizeof(s_client_id))!=ESP_OK) return ESP_FAIL;
    cfg_service_get_str(CFG_KEY_MQTT_HOST,host,sizeof(host),"mqtt://broker.emqx.io"); cfg_service_get_str(CFG_KEY_MQTT_USERNAME,user,sizeof(user),""); cfg_service_get_str(CFG_KEY_MQTT_PASSWORD,pass,sizeof(pass),""); cfg_service_get_str(CFG_KEY_MQTT_BASE_TOPIC,base,sizeof(base),"ymos/v1"); port=cfg_service_get_int(CFG_KEY_MQTT_PORT,1883); mqtt_topics_init(base,s_device);
    esp_mqtt_client_config_t c={0}; c.broker.address.uri=host; c.broker.address.port=port; c.credentials.client_id=s_client_id; c.credentials.username=user; c.credentials.authentication.password=pass; c.session.keepalive=60; c.session.last_will.topic=mqtt_topic_status(); c.session.last_will.msg="{\"version\":1,\"type\":\"status\",\"payload\":{\"state\":\"offline\"}}"; c.session.last_will.qos=1; c.session.last_will.retain=true;
    s_client=esp_mqtt_client_init(&c);
    if(!s_client) return ESP_ERR_NO_MEM;
    esp_err_t err=esp_mqtt_client_register_event(s_client,ESP_EVENT_ANY_ID,on_event,NULL);
    if(err==ESP_OK) err=esp_mqtt_client_start(s_client);
    /* 启动失败清空句柄，让下一次 GOT_IP 仍可重试。 */
    if(err!=ESP_OK) { esp_mqtt_client_destroy(s_client); s_client=NULL; }
    return err;
}
esp_err_t mqtt_service_stop(void){ if(!s_client)return ESP_OK; s_connected=false; esp_mqtt_client_stop(s_client); esp_mqtt_client_destroy(s_client); s_client=NULL; return ESP_OK; }
bool mqtt_service_is_connected(void){return s_connected;}
