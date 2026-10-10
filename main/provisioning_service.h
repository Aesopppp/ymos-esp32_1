#ifndef PROVISIONING_SERVICE_H
#define PROVISIONING_SERVICE_H
#include "esp_err.h"
#include "event_bus.h"
esp_err_t provisioning_service_init(event_bus_t bus);
esp_err_t provisioning_service_start(void);
esp_err_t provisioning_service_stop(void);
bool provisioning_service_is_active(void);
void provisioning_service_rollback(void);
#endif
