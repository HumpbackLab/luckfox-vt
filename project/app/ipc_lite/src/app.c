#include "app.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "file_sink.h"
#include "isp.h"
#include "log.h"
#include "mpi_pipeline.h"
#include "rtmp_sink.h"
#include "rtsp_sink.h"
#include "wifi_status.h"

static volatile sig_atomic_t g_stop_requested = 0;

typedef struct {
  uint64_t frames;
  uint64_t bytes;
  struct timespec timestamp;
} IPC_LITE_PERIODIC_STATS;

static void handle_signal(int signo) {
  (void)signo;
  g_stop_requested = 1;
}

static void install_signal_handlers(void) {
  struct sigaction action;

  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);

  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end) {
  double seconds = (double)(end->tv_sec - start->tv_sec);
  seconds += (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
  return seconds;
}

static void log_config_summary(const IPC_LITE_CONFIG *config) {
  IPC_LITE_LOGI("app",
                "config=%s codec=%s size=%dx%d fps=%d bitrate=%dkbps gop=%d",
                config->config_path, ipc_lite_codec_name(config->video.codec),
                config->video.width, config->video.height, config->video.fps,
                config->video.bitrate_kbps, config->video.gop);
  IPC_LITE_LOGI("app", "aiq=%s iq_dir=%s rtsp=%s file=%s",
                config->isp.enable_aiq ? "on" : "off", config->isp.iq_dir,
                config->rtsp.enable ? "on" : "off",
                config->file_output.enable ? config->file_output.path : "off");
}

static void log_periodic_stats(const IPC_LITE_CONFIG *config,
                               IPC_LITE_MPI_PIPELINE *pipeline,
                               IPC_LITE_PERIODIC_STATS *previous) {
  IPC_LITE_PIPELINE_STATS_SNAPSHOT snapshot;
  IPC_LITE_WIFI_STATUS wifi_status;
  struct timespec now;
  uint64_t frame_delta = 0;
  uint64_t byte_delta = 0;
  double interval_seconds = 0.0;
  double fps = 0.0;
  double kbps = 0.0;

  memset(&snapshot, 0, sizeof(snapshot));
  clock_gettime(CLOCK_MONOTONIC, &now);
  ipc_lite_pipeline_get_stats(pipeline, &snapshot);

  frame_delta = snapshot.frames - previous->frames;
  byte_delta = snapshot.bytes - previous->bytes;
  interval_seconds = elapsed_seconds(&previous->timestamp, &now);
  if (interval_seconds > 0.0) {
    fps = (double)frame_delta / interval_seconds;
    kbps = ((double)byte_delta * 8.0) / interval_seconds / 1000.0;
  }

  if (config->wifi.enable_status &&
      ipc_lite_wifi_status_read(config->wifi.ifname, &wifi_status) == 0) {
    IPC_LITE_LOGI("stats",
                  "frames=%llu bytes=%llu fps=%.2f kbps=%.2f last_len=%u "
                  "timeouts=%llu errors=%llu wifi=%s carrier=%d ip=%s",
                  (unsigned long long)snapshot.frames,
                  (unsigned long long)snapshot.bytes, fps, kbps,
                  snapshot.last_len, (unsigned long long)snapshot.timeouts,
                  (unsigned long long)snapshot.errors, wifi_status.operstate,
                  wifi_status.carrier ? 1 : 0,
                  wifi_status.ipv4[0] ? wifi_status.ipv4 : "-");
  } else {
    IPC_LITE_LOGI("stats",
                  "frames=%llu bytes=%llu fps=%.2f kbps=%.2f last_len=%u "
                  "timeouts=%llu errors=%llu",
                  (unsigned long long)snapshot.frames,
                  (unsigned long long)snapshot.bytes, fps, kbps,
                  snapshot.last_len, (unsigned long long)snapshot.timeouts,
                  (unsigned long long)snapshot.errors);
  }

  previous->frames = snapshot.frames;
  previous->bytes = snapshot.bytes;
  previous->timestamp = now;
}

int ipc_lite_run(const char *config_path) {
  IPC_LITE_CONFIG config;
  IPC_LITE_ISP_CONTEXT isp;
  IPC_LITE_MPI_PIPELINE pipeline;
  IPC_LITE_STREAM_SINK sinks[3];
  IPC_LITE_PERIODIC_STATS periodic_stats;
  struct timespec last_log_at;
  int ret = -1;

  memset(&config, 0, sizeof(config));
  memset(&isp, 0, sizeof(isp));
  memset(&pipeline, 0, sizeof(pipeline));
  memset(&periodic_stats, 0, sizeof(periodic_stats));
  memset(&last_log_at, 0, sizeof(last_log_at));

  if (ipc_lite_config_load_file(&config, config_path) != 0) {
    fprintf(stderr, "failed to load config: %s\n", config_path);
    return -1;
  }

  ipc_lite_log_init(config.app.log_level);
  log_config_summary(&config);

  install_signal_handlers();

  ipc_lite_file_sink_init(&sinks[0]);
  ipc_lite_rtsp_sink_init(&sinks[1]);
  ipc_lite_rtmp_sink_init(&sinks[2]);

  if (ipc_lite_isp_start(&isp, &config) != 0) {
    IPC_LITE_LOGE("app", "failed to start isp");
    goto cleanup;
  }

  if (ipc_lite_pipeline_start(&pipeline, &config, sinks,
                              sizeof(sinks) / sizeof(sinks[0])) != 0) {
    IPC_LITE_LOGE("app", "failed to start pipeline");
    goto cleanup;
  }

  clock_gettime(CLOCK_MONOTONIC, &periodic_stats.timestamp);
  last_log_at = periodic_stats.timestamp;

  IPC_LITE_LOGI("app", "ipc_lite started in foreground, press Ctrl+C to stop");

  while (!g_stop_requested) {
    struct timespec now;

    usleep(200000);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (elapsed_seconds(&last_log_at, &now) >= config.app.stats_interval_sec) {
      log_periodic_stats(&config, &pipeline, &periodic_stats);
      last_log_at = now;
    }
  }

  IPC_LITE_LOGI("app", "stop requested, shutting down");
  ret = 0;

cleanup:
  ipc_lite_pipeline_request_stop(&pipeline);
  ipc_lite_pipeline_stop(&pipeline);
  ipc_lite_isp_stop(&isp);
  return ret;
}

