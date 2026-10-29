/**
 * @file acos_reminder_service.h
 * @brief 独立的 ACOS MQTT 提醒通道，不替换 YMOS MQTT 管理通道。
 *
 * init/start/stop/take/finish 仅由主任务调用。MQTT 回调只校验和入队，
 * 不调用 WebSocket/音频 API。stop 销毁网络客户端但保留待播消息与去重记录。
 */
#ifndef ACOS_REMINDER_SERVICE_H
#define ACOS_REMINDER_SERVICE_H

#include "esp_err.h"
#include "reminder_message.h"

esp_err_t acos_reminder_service_init(void);
esp_err_t acos_reminder_service_start(void);
esp_err_t acos_reminder_service_stop(void);
/* 借用指针，finish 前有效；网络回调不会修改/释放正在播放的消息。 */
const reminder_message_t *acos_reminder_service_take(void);
void acos_reminder_service_finish(reminder_outcome_t outcome);

#endif
