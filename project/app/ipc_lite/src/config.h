#ifndef IPC_LITE_CONFIG_H
#define IPC_LITE_CONFIG_H

#include <stdbool.h>

#define IPC_LITE_PATH_MAX 256
#define IPC_LITE_NAME_MAX 64
#define IPC_LITE_IFNAME_MAX 32

typedef enum {
  IPC_LITE_LOG_LEVEL_ERROR = 0,
  IPC_LITE_LOG_LEVEL_WARN = 1,
  IPC_LITE_LOG_LEVEL_INFO = 2,
  IPC_LITE_LOG_LEVEL_DEBUG = 3,
} IPC_LITE_LOG_LEVEL;

typedef enum {
  IPC_LITE_CODEC_H264 = 0,
  IPC_LITE_CODEC_H265 = 1,
} IPC_LITE_CODEC;

typedef struct {
  char name[IPC_LITE_NAME_MAX];
  IPC_LITE_LOG_LEVEL log_level;
  int stats_interval_sec;
} IPC_LITE_APP_SECTION;

typedef struct {
  bool enable_aiq;
  int cam_id;
  int hdr_mode;
  char iq_dir[IPC_LITE_PATH_MAX];
} IPC_LITE_ISP_SECTION;

typedef struct {
  int width;
  int height;
  int fps;
  int gop;
  int bitrate_kbps;
  IPC_LITE_CODEC codec;
  int vi_channel;
  int venc_channel;
  int venc_timeout_ms;
} IPC_LITE_VIDEO_SECTION;

typedef struct {
  bool enable;
  char path[IPC_LITE_PATH_MAX];
} IPC_LITE_FILE_OUTPUT_SECTION;

typedef struct {
  bool enable;
  int port;
  char path[IPC_LITE_PATH_MAX];
} IPC_LITE_RTSP_SECTION;

typedef struct {
  bool enable;
  char url[IPC_LITE_PATH_MAX];
} IPC_LITE_RTMP_SECTION;

typedef struct {
  bool enable_status;
  char ifname[IPC_LITE_IFNAME_MAX];
} IPC_LITE_WIFI_SECTION;

typedef struct {
  int dump_first_frames;
} IPC_LITE_DEBUG_SECTION;

typedef struct {
  char config_path[IPC_LITE_PATH_MAX];
  IPC_LITE_APP_SECTION app;
  IPC_LITE_ISP_SECTION isp;
  IPC_LITE_VIDEO_SECTION video;
  IPC_LITE_FILE_OUTPUT_SECTION file_output;
  IPC_LITE_RTSP_SECTION rtsp;
  IPC_LITE_RTMP_SECTION rtmp;
  IPC_LITE_WIFI_SECTION wifi;
  IPC_LITE_DEBUG_SECTION debug;
} IPC_LITE_CONFIG;

void ipc_lite_config_set_defaults(IPC_LITE_CONFIG *config);
int ipc_lite_config_load_file(IPC_LITE_CONFIG *config, const char *path);
const char *ipc_lite_codec_name(IPC_LITE_CODEC codec);
const char *ipc_lite_log_level_name(IPC_LITE_LOG_LEVEL level);

#endif

