#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static IPC_LITE_LOG_LEVEL g_log_level = IPC_LITE_LOG_LEVEL_INFO;

void ipc_lite_log_init(IPC_LITE_LOG_LEVEL level) { g_log_level = level; }

void ipc_lite_log_write(IPC_LITE_LOG_LEVEL level, const char *module,
                        const char *fmt, ...) {
  va_list args;
  struct timespec ts;
  struct tm local_tm;
  char time_buf[64];
  long millis = 0;

  if (level > g_log_level) {
    return;
  }

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &local_tm);
  millis = ts.tv_nsec / 1000000L;
  snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
           local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
           local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, millis);

  fprintf(stdout, "%s [%s] %s: ", time_buf, ipc_lite_log_level_name(level),
          module);
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

