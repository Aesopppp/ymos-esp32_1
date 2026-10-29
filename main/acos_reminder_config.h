/** ACOS 提醒的编译期默认值。NVS 优先；本机凭据文件不进入 Git。 */
#ifndef ACOS_REMINDER_CONFIG_H
#define ACOS_REMINDER_CONFIG_H

#include <stdbool.h>

#if __has_include("acos_reminder_local_config.h")
#include "acos_reminder_local_config.h"
#endif
#ifndef ACOS_REMINDER_ENABLED
#define ACOS_REMINDER_ENABLED true
#endif
#ifndef ACOS_REMINDER_URI
#define ACOS_REMINDER_URI "mqtt://47.94.139.172:11116"
#endif
#ifndef ACOS_REMINDER_API_KEY
#define ACOS_REMINDER_API_KEY ""
#endif
#ifndef ACOS_REMINDER_USE_WS_TOKEN
#define ACOS_REMINDER_USE_WS_TOKEN true
#endif
#ifndef ACOS_REMINDER_USERNAME
#define ACOS_REMINDER_USERNAME ""
#endif
#ifndef ACOS_REMINDER_PASSWORD
#define ACOS_REMINDER_PASSWORD ""
#endif
#endif
