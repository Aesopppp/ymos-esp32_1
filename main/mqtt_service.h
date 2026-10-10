#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H
#include "esp_err.h"
#include "event_bus.h"
esp_err_t mqtt_service_init(event_bus_t bus);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);
bool mqtt_service_is_connected(void);
#endif
