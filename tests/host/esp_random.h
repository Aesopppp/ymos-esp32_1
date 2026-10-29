/* 只供 Windows 主机测试编译协议 builder；固件使用 ESP-IDF 原头文件。 */
#ifndef HOST_ESP_RANDOM_H
#define HOST_ESP_RANDOM_H
#include <stdint.h>
uint32_t esp_random(void);
#endif
