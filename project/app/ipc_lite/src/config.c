#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
  char *start = text;
  char *end = NULL;

  while (*start && isspace((unsigned char)*start)) {
    ++start;
  }

  if (*start == '\0') {
    return start;
  }

  end = start + strlen(start) - 1;
  while (end > start && isspace((unsigned char)*end)) {
    *end = '\0';
    --end;
  }

  return start;
}

static int parse_bool(const char *value, bool *result) {
  if (!strcasecmp(value, "1") || !strcasecmp(value, "true") ||
      !strcasecmp(value, "yes") || !strcasecmp(value, "on")) {
    *result = true;
    return 0;
  }

  if (!strcasecmp(value, "0") || !strcasecmp(value, "false") ||
      !strcasecmp(value, "no") || !strcasecmp(value, "off")) {
    *result = false;
    return 0;
  }

  return -1;
}

static int parse_int(const char *value, int *result) {
  char *end = NULL;
  long parsed = strtol(value, &end, 10);

  if (end == value || *trim(end) != '\0') {
    return -1;
  }

  *result = (int)parsed;
  return 0;
}

static int parse_log_level(const char *value, IPC_LITE_LOG_LEVEL *level) {
  int numeric = 0;

  if (!parse_int(value, &numeric) && numeric >= IPC_LITE_LOG_LEVEL_ERROR &&
      numeric <= IPC_LITE_LOG_LEVEL_DEBUG) {
    *level = (IPC_LITE_LOG_LEVEL)numeric;
    return 0;
  }

  if (!strcasecmp(value, "ERROR")) {
    *level = IPC_LITE_LOG_LEVEL_ERROR;
    return 0;
  }

  if (!strcasecmp(value, "WARN") || !strcasecmp(value, "WARNING")) {
    *level = IPC_LITE_LOG_LEVEL_WARN;
    return 0;
  }

  if (!strcasecmp(value, "INFO")) {
    *level = IPC_LITE_LOG_LEVEL_INFO;
    return 0;
  }

  if (!strcasecmp(value, "DEBUG")) {
    *level = IPC_LITE_LOG_LEVEL_DEBUG;
    return 0;
  }

  return -1;
}

static int parse_codec(const char *value, IPC_LITE_CODEC *codec) {
  if (!strcasecmp(value, "h264")) {
    *codec = IPC_LITE_CODEC_H264;
    return 0;
  }

  if (!strcasecmp(value, "h265") || !strcasecmp(value, "hevc")) {
    *codec = IPC_LITE_CODEC_H265;
    return 0;
  }

  return -1;
}

static void set_string(char *dst, size_t dst_size, const char *src) {
  snprintf(dst, dst_size, "%s", src);
}

void ipc_lite_config_set_defaults(IPC_LITE_CONFIG *config) {
  memset(config, 0, sizeof(*config));

  set_string(config->app.name, sizeof(config->app.name), "ipc_lite");
  config->app.log_level = IPC_LITE_LOG_LEVEL_INFO;
  config->app.stats_interval_sec = 2;

  config->isp.enable_aiq = true;
  config->isp.cam_id = 0;
  config->isp.hdr_mode = 0;
  set_string(config->isp.iq_dir, sizeof(config->isp.iq_dir),
             "/oem/usr/share/iqfiles");

  config->video.width = 704;
  config->video.height = 576;
  config->video.max_width = 704;
  config->video.max_height = 576;
  config->video.fps = 25;
  config->video.gop = 50;
  config->video.bitrate_kbps = 512;
  config->video.codec = IPC_LITE_CODEC_H265;
  config->video.vi_channel = 1;
  config->video.venc_channel = 1;
  config->video.input_buffer_count = 2;
  config->video.venc_buffer_count = 4;
  config->video.venc_buffer_size = 202752;
  config->video.enable_refer_buffer_share = true;
  config->video.venc_timeout_ms = 1000;

  config->file_output.enable = true;
  set_string(config->file_output.path, sizeof(config->file_output.path),
             "/tmp/ipc_lite.h265");

  config->rtsp.enable = false;
  config->rtsp.port = 554;
  set_string(config->rtsp.path, sizeof(config->rtsp.path), "/live/0");

  config->rtmp.enable = false;
  set_string(config->rtmp.url, sizeof(config->rtmp.url),
             "rtmp://127.0.0.1:1935/live/mainstream");

  config->wifi.enable_status = true;
  set_string(config->wifi.ifname, sizeof(config->wifi.ifname), "wlan0");

  config->debug.dump_first_frames = 5;
}

static int apply_value(IPC_LITE_CONFIG *config, const char *section,
                       const char *key, const char *value) {
  bool bool_value = false;

  if (!strcmp(section, "app")) {
    if (!strcmp(key, "name")) {
      set_string(config->app.name, sizeof(config->app.name), value);
      return 0;
    }
    if (!strcmp(key, "log_level")) {
      return parse_log_level(value, &config->app.log_level);
    }
    if (!strcmp(key, "stats_interval_sec")) {
      return parse_int(value, &config->app.stats_interval_sec);
    }
  } else if (!strcmp(section, "isp")) {
    if (!strcmp(key, "enable_aiq")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->isp.enable_aiq = bool_value;
      return 0;
    }
    if (!strcmp(key, "cam_id")) {
      return parse_int(value, &config->isp.cam_id);
    }
    if (!strcmp(key, "hdr_mode")) {
      return parse_int(value, &config->isp.hdr_mode);
    }
    if (!strcmp(key, "iq_dir")) {
      set_string(config->isp.iq_dir, sizeof(config->isp.iq_dir), value);
      return 0;
    }
  } else if (!strcmp(section, "video")) {
    if (!strcmp(key, "width")) {
      return parse_int(value, &config->video.width);
    }
    if (!strcmp(key, "height")) {
      return parse_int(value, &config->video.height);
    }
    if (!strcmp(key, "max_width")) {
      return parse_int(value, &config->video.max_width);
    }
    if (!strcmp(key, "max_height")) {
      return parse_int(value, &config->video.max_height);
    }
    if (!strcmp(key, "fps")) {
      return parse_int(value, &config->video.fps);
    }
    if (!strcmp(key, "gop")) {
      return parse_int(value, &config->video.gop);
    }
    if (!strcmp(key, "bitrate_kbps")) {
      return parse_int(value, &config->video.bitrate_kbps);
    }
    if (!strcmp(key, "codec")) {
      return parse_codec(value, &config->video.codec);
    }
    if (!strcmp(key, "vi_channel")) {
      return parse_int(value, &config->video.vi_channel);
    }
    if (!strcmp(key, "venc_channel")) {
      return parse_int(value, &config->video.venc_channel);
    }
    if (!strcmp(key, "input_buffer_count")) {
      return parse_int(value, &config->video.input_buffer_count);
    }
    if (!strcmp(key, "venc_buffer_count")) {
      return parse_int(value, &config->video.venc_buffer_count);
    }
    if (!strcmp(key, "venc_buffer_size")) {
      return parse_int(value, &config->video.venc_buffer_size);
    }
    if (!strcmp(key, "enable_refer_buffer_share")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->video.enable_refer_buffer_share = bool_value;
      return 0;
    }
    if (!strcmp(key, "venc_timeout_ms")) {
      return parse_int(value, &config->video.venc_timeout_ms);
    }
  } else if (!strcmp(section, "output.file")) {
    if (!strcmp(key, "enable")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->file_output.enable = bool_value;
      return 0;
    }
    if (!strcmp(key, "path")) {
      set_string(config->file_output.path, sizeof(config->file_output.path),
                 value);
      return 0;
    }
  } else if (!strcmp(section, "output.rtsp")) {
    if (!strcmp(key, "enable")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->rtsp.enable = bool_value;
      return 0;
    }
    if (!strcmp(key, "port")) {
      return parse_int(value, &config->rtsp.port);
    }
    if (!strcmp(key, "path")) {
      set_string(config->rtsp.path, sizeof(config->rtsp.path), value);
      return 0;
    }
  } else if (!strcmp(section, "output.rtmp")) {
    if (!strcmp(key, "enable")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->rtmp.enable = bool_value;
      return 0;
    }
    if (!strcmp(key, "url")) {
      set_string(config->rtmp.url, sizeof(config->rtmp.url), value);
      return 0;
    }
  } else if (!strcmp(section, "wifi")) {
    if (!strcmp(key, "enable_status")) {
      if (parse_bool(value, &bool_value)) {
        return -1;
      }
      config->wifi.enable_status = bool_value;
      return 0;
    }
    if (!strcmp(key, "ifname")) {
      set_string(config->wifi.ifname, sizeof(config->wifi.ifname), value);
      return 0;
    }
  } else if (!strcmp(section, "debug")) {
    if (!strcmp(key, "dump_first_frames")) {
      return parse_int(value, &config->debug.dump_first_frames);
    }
  }

  return 0;
}

static int validate(const IPC_LITE_CONFIG *config) {
  if (config->video.width <= 0 || config->video.height <= 0 ||
      config->video.max_width <= 0 || config->video.max_height <= 0 ||
      config->video.fps <= 0 || config->video.gop <= 0 ||
      config->video.bitrate_kbps <= 0 ||
      config->video.input_buffer_count <= 0 ||
      config->video.venc_buffer_count <= 0 ||
      config->video.venc_buffer_size <= 0 ||
      config->video.venc_timeout_ms <= 0) {
    return -1;
  }

  if (config->video.width > config->video.max_width ||
      config->video.height > config->video.max_height) {
    return -1;
  }

  if (config->app.stats_interval_sec <= 0) {
    return -1;
  }

  if (config->rtsp.enable && config->rtsp.port <= 0) {
    return -1;
  }

  return 0;
}

int ipc_lite_config_load_file(IPC_LITE_CONFIG *config, const char *path) {
  FILE *fp = NULL;
  char section[64] = "";
  char line[512];
  int line_number = 0;

  ipc_lite_config_set_defaults(config);
  set_string(config->config_path, sizeof(config->config_path), path);

  fp = fopen(path, "r");
  if (!fp) {
    return -1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *current = NULL;
    char *equal = NULL;
    char *key = NULL;
    char *value = NULL;
    ++line_number;

    current = trim(line);
    if (*current == '\0' || *current == ';' || *current == '#') {
      continue;
    }

    if (*current == '[') {
      char *right = strchr(current, ']');
      if (!right) {
        fclose(fp);
        return -1;
      }
      *right = '\0';
      set_string(section, sizeof(section), trim(current + 1));
      continue;
    }

    equal = strchr(current, '=');
    if (!equal) {
      fclose(fp);
      return -1;
    }

    *equal = '\0';
    key = trim(current);
    value = trim(equal + 1);

    if (apply_value(config, section, key, value) != 0) {
      fclose(fp);
      return -1;
    }
  }

  fclose(fp);
  return validate(config);
}

const char *ipc_lite_codec_name(IPC_LITE_CODEC codec) {
  switch (codec) {
    case IPC_LITE_CODEC_H264:
      return "H264";
    case IPC_LITE_CODEC_H265:
      return "H265";
    default:
      return "UNKNOWN";
  }
}

const char *ipc_lite_log_level_name(IPC_LITE_LOG_LEVEL level) {
  switch (level) {
    case IPC_LITE_LOG_LEVEL_ERROR:
      return "ERROR";
    case IPC_LITE_LOG_LEVEL_WARN:
      return "WARN";
    case IPC_LITE_LOG_LEVEL_INFO:
      return "INFO";
    case IPC_LITE_LOG_LEVEL_DEBUG:
      return "DEBUG";
    default:
      return "UNKNOWN";
  }
}
