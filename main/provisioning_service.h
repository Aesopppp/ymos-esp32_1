#ifndef PROVISIONING_SERVICE_H
#define PROVISIONING_SERVICE_H
#include "esp_err.h"
#include "event_bus.h"
esp_err_t provisioning_service_init(event_bus_t bus);
esp_err_t provisioning_service_start(void);
esp_err_t provisioning_service_stop(void);
bool provisioning_service_is_active(void);
/* DONE 事件携带此代次，避免快速进出时旧提交关闭新的配网会话。 */
uint32_t provisioning_service_generation(void);
/* 仅主任务查询，防止旧超时事件关闭新一轮配网。 */
bool provisioning_service_timed_out(void);
/* 主任务在 GOT_IP 确认、连接失败回滚；进入/退出配网本身不清除凭据。 */
void provisioning_service_confirm(void);
void provisioning_service_rollback(void);
#endif
