#include "wifi_status.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void trim_newline(char *text) {
  char *newline = strchr(text, '\n');
  if (newline) {
    *newline = '\0';
  }
}

static int read_text_file(const char *path, char *buffer, size_t size) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    return -1;
  }

  if (!fgets(buffer, (int)size, fp)) {
    fclose(fp);
    return -1;
  }

  fclose(fp);
  trim_newline(buffer);
  return 0;
}

int ipc_lite_wifi_status_read(const char *ifname, IPC_LITE_WIFI_STATUS *status) {
  char path[128];
  char buffer[64];
  int fd = -1;
  struct ifreq ifr;

  memset(status, 0, sizeof(*status));

  snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
  if (read_text_file(path, status->operstate, sizeof(status->operstate)) != 0) {
    return -1;
  }

  status->exists = true;

  snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
  if (read_text_file(path, buffer, sizeof(buffer)) == 0) {
    status->carrier = (buffer[0] == '1');
  }

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 0;
  }

  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

  if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    inet_ntop(AF_INET, &sin->sin_addr, status->ipv4, sizeof(status->ipv4));
  }

  close(fd);
  return 0;
}

