#ifndef BUTTON_SERVICE_H
#define BUTTON_SERVICE_H

#include "esp_err.h"

/* system_init 调用一次；持有 ADC1_CH4，任务只扫描、消抖、发布无 payload 事件。
 * 不在扫描任务操作网络、Codec 或 NVS。重复初始化为 no-op。
 */
esp_err_t button_service_init(void);

#endif
