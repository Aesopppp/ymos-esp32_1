/* 只替换协议 builder 的时钟依赖，不模拟 RTOS 或音频外设。 */
#ifndef HOST_ESP_TIMER_H
#define HOST_ESP_TIMER_H
#include <stdint.h>
int64_t esp_timer_get_time(void);
#endif
