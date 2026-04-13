#ifndef IPC_LITE_LOG_H
#define IPC_LITE_LOG_H

#include "config.h"

void ipc_lite_log_init(IPC_LITE_LOG_LEVEL level);
void ipc_lite_log_write(IPC_LITE_LOG_LEVEL level, const char *module,
                        const char *fmt, ...);

#define IPC_LITE_LOGE(module, fmt, ...) \
  ipc_lite_log_write(IPC_LITE_LOG_LEVEL_ERROR, module, fmt, ##__VA_ARGS__)
#define IPC_LITE_LOGW(module, fmt, ...) \
  ipc_lite_log_write(IPC_LITE_LOG_LEVEL_WARN, module, fmt, ##__VA_ARGS__)
#define IPC_LITE_LOGI(module, fmt, ...) \
  ipc_lite_log_write(IPC_LITE_LOG_LEVEL_INFO, module, fmt, ##__VA_ARGS__)
#define IPC_LITE_LOGD(module, fmt, ...) \
  ipc_lite_log_write(IPC_LITE_LOG_LEVEL_DEBUG, module, fmt, ##__VA_ARGS__)

#endif

