#ifndef IPC_LITE_WIFI_STATUS_H
#define IPC_LITE_WIFI_STATUS_H

#include <stdbool.h>

typedef struct {
  bool exists;
  bool carrier;
  char operstate[32];
  char ipv4[64];
} IPC_LITE_WIFI_STATUS;

int ipc_lite_wifi_status_read(const char *ifname, IPC_LITE_WIFI_STATUS *status);

#endif

