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
#include "udp_rtp_sink.h"
#include "wifi_status.h"

static volatile sig_atomic_t g_stop_requested = 0;

typedef struct {
  uint64_t frames;
  uint64_t bytes;
  uint64_t timeouts;
  struct timespec timestamp;
} IPC_LITE_PERIODIC_STATS;

typedef struct {
  IPC_LITE_PIPELINE_STATS_SNAPSHOT snapshot;
  struct timespec timestamp;
  uint64_t frame_delta;
  uint64_t byte_delta;
  uint64_t timeout_delta;
  double interval_seconds;
  double fps;
  double kbps;
} IPC_LITE_STATS_SAMPLE;

typedef struct {
  unsigned int restart_count;
  unsigned int preflight_healthy_intervals;
} IPC_LITE_HEALTH_STATE;

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

static void sample_pipeline_stats(IPC_LITE_MPI_PIPELINE *pipeline,
                                  IPC_LITE_PERIODIC_STATS *previous,
                                  IPC_LITE_STATS_SAMPLE *sample) {
  memset(sample, 0, sizeof(*sample));
  clock_gettime(CLOCK_MONOTONIC, &sample->timestamp);
  ipc_lite_pipeline_get_stats(pipeline, &sample->snapshot);

  sample->frame_delta = sample->snapshot.frames - previous->frames;
  sample->byte_delta = sample->snapshot.bytes - previous->bytes;
  sample->timeout_delta = sample->snapshot.timeouts - previous->timeouts;
  sample->interval_seconds =
      elapsed_seconds(&previous->timestamp, &sample->timestamp);

  if (sample->interval_seconds > 0.0) {
    sample->fps = (double)sample->frame_delta / sample->interval_seconds;
    sample->kbps =
        ((double)sample->byte_delta * 8.0) / sample->interval_seconds / 1000.0;
  }
}

static void log_config_summary(const IPC_LITE_CONFIG *config) {
  IPC_LITE_LOGI("app",
                "config=%s codec=%s size=%dx%d max=%dx%d fps=%d "
                "bitrate=%dkbps gop=%d vi=%d venc=%d",
                config->config_path, ipc_lite_codec_name(config->video.codec),
                config->video.width, config->video.height,
                config->video.max_width, config->video.max_height,
                config->video.fps, config->video.bitrate_kbps,
                config->video.gop, config->video.vi_channel,
                config->video.venc_channel);
  IPC_LITE_LOGI("app",
                "aiq=%s iq_dir=%s in_buf=%d venc_buf=%d/%d ref_share=%s "
                "sensor_fps=%s scene=%d rtsp=%s udp_rtp=%s file=%s",
                config->isp.enable_aiq ? "on" : "off", config->isp.iq_dir,
                config->video.input_buffer_count,
                config->video.venc_buffer_count,
                config->video.venc_buffer_size,
                config->video.enable_refer_buffer_share ? "on" : "off",
                config->video.sync_sensor_fps ? "sync" : "keep",
                config->video.scene_mode,
                config->rtsp.enable ? "on" : "off",
                config->udp_rtp.enable ? "on" : "off",
                config->file_output.enable ? config->file_output.path : "off");
  IPC_LITE_LOGI("app",
                "idr_start=%s idr_rtsp=%s motion_deblur=%s(%d) "
                "motion_static=%s slice=%s/%d/%d wakeup=%d max_stream=%d",
                config->video.request_idr_on_start ? "on" : "off",
                config->video.request_idr_on_rtsp_enable ? "on" : "off",
                config->video.enable_motion_deblur ? "on" : "off",
                config->video.motion_deblur_strength,
                config->video.enable_motion_static_switch ? "on" : "off",
                config->video.enable_slice_split ? "on" : "off",
                config->video.slice_split_mode,
                config->video.slice_split_size,
                config->video.poll_wakeup_frame_count,
                config->video.max_stream_count);
}

static void log_periodic_stats(const IPC_LITE_CONFIG *config,
                               const IPC_LITE_STATS_SAMPLE *sample) {
  IPC_LITE_WIFI_STATUS wifi_status;

  if (config->wifi.enable_status &&
      ipc_lite_wifi_status_read(config->wifi.ifname, &wifi_status) == 0) {
    IPC_LITE_LOGI("stats",
                  "frames=%llu bytes=%llu fps=%.2f kbps=%.2f last_len=%u "
                  "timeouts=%llu errors=%llu wifi=%s carrier=%d ip=%s",
                  (unsigned long long)sample->snapshot.frames,
                  (unsigned long long)sample->snapshot.bytes, sample->fps,
                  sample->kbps, sample->snapshot.last_len,
                  (unsigned long long)sample->snapshot.timeouts,
                  (unsigned long long)sample->snapshot.errors,
                  wifi_status.operstate,
                  wifi_status.carrier ? 1 : 0,
                  wifi_status.ipv4[0] ? wifi_status.ipv4 : "-");
  } else {
    IPC_LITE_LOGI("stats",
                  "frames=%llu bytes=%llu fps=%.2f kbps=%.2f last_len=%u "
                  "timeouts=%llu errors=%llu",
                  (unsigned long long)sample->snapshot.frames,
                  (unsigned long long)sample->snapshot.bytes, sample->fps,
                  sample->kbps, sample->snapshot.last_len,
                  (unsigned long long)sample->snapshot.timeouts,
                  (unsigned long long)sample->snapshot.errors);
  }
}

static void commit_periodic_stats(IPC_LITE_PERIODIC_STATS *previous,
                                  const IPC_LITE_STATS_SAMPLE *sample) {
  previous->frames = sample->snapshot.frames;
  previous->bytes = sample->snapshot.bytes;
  previous->timeouts = sample->snapshot.timeouts;
  previous->timestamp = sample->timestamp;
}

static bool sample_requires_restart(const IPC_LITE_STATS_SAMPLE *sample) {
  const bool no_stream =
      sample->frame_delta == 0 && sample->timeout_delta > 0 &&
      sample->snapshot.last_len == 0;
  const bool tiny_packets =
      sample->fps >= 10.0 && sample->kbps > 0.0 && sample->kbps < 20.0 &&
      sample->snapshot.last_len > 0 && sample->snapshot.last_len <= 32;

  return no_stream || tiny_packets;
}

static void reset_periodic_stats(IPC_LITE_MPI_PIPELINE *pipeline,
                                 IPC_LITE_PERIODIC_STATS *periodic_stats,
                                 struct timespec *last_log_at) {
  IPC_LITE_PIPELINE_STATS_SNAPSHOT snapshot;

  memset(periodic_stats, 0, sizeof(*periodic_stats));
  memset(&snapshot, 0, sizeof(snapshot));
  clock_gettime(CLOCK_MONOTONIC, &periodic_stats->timestamp);
  ipc_lite_pipeline_get_stats(pipeline, &snapshot);
  periodic_stats->frames = snapshot.frames;
  periodic_stats->bytes = snapshot.bytes;
  periodic_stats->timeouts = snapshot.timeouts;
  *last_log_at = periodic_stats->timestamp;
}

static bool config_uses_network_sinks(const IPC_LITE_CONFIG *config) {
  return config->rtsp.enable || config->rtmp.enable || config->udp_rtp.enable;
}

static int start_stream_stack(const IPC_LITE_CONFIG *config,
                              IPC_LITE_ISP_CONTEXT *isp,
                              IPC_LITE_MPI_PIPELINE *pipeline,
                              IPC_LITE_STREAM_SINK *sinks,
                              size_t sink_count,
                              IPC_LITE_PERIODIC_STATS *periodic_stats,
                              struct timespec *last_log_at) {
  if (ipc_lite_isp_start(isp, config) != 0) {
    IPC_LITE_LOGE("app", "failed to start isp");
    return -1;
  }

  if (ipc_lite_pipeline_start(pipeline, config, sinks, sink_count) != 0) {
    IPC_LITE_LOGE("app", "failed to start pipeline");
    ipc_lite_isp_stop(isp);
    return -1;
  }

  reset_periodic_stats(pipeline, periodic_stats, last_log_at);
  if (config->video.request_idr_on_start) {
    ipc_lite_pipeline_request_idr(pipeline, false);
  }
  return 0;
}

static void stop_stream_stack(IPC_LITE_ISP_CONTEXT *isp,
                              IPC_LITE_MPI_PIPELINE *pipeline) {
  ipc_lite_pipeline_request_stop(pipeline);
  ipc_lite_pipeline_stop(pipeline);
  ipc_lite_isp_stop(isp);
}

int ipc_lite_run(const char *config_path) {
  IPC_LITE_CONFIG config;
  IPC_LITE_CONFIG active_config;
  IPC_LITE_ISP_CONTEXT isp;
  IPC_LITE_MPI_PIPELINE pipeline;
  IPC_LITE_STREAM_SINK sinks[4];
  IPC_LITE_PERIODIC_STATS periodic_stats;
  IPC_LITE_HEALTH_STATE health_state;
  IPC_LITE_STATS_SAMPLE stats_sample;
  struct timespec last_log_at;
  int ret = -1;

  memset(&config, 0, sizeof(config));
  memset(&active_config, 0, sizeof(active_config));
  memset(&isp, 0, sizeof(isp));
  memset(&pipeline, 0, sizeof(pipeline));
  memset(&periodic_stats, 0, sizeof(periodic_stats));
  memset(&health_state, 0, sizeof(health_state));
  memset(&stats_sample, 0, sizeof(stats_sample));
  memset(&last_log_at, 0, sizeof(last_log_at));

  if (ipc_lite_config_load_file(&config, config_path) != 0) {
    fprintf(stderr, "failed to load config: %s\n", config_path);
    return -1;
  }

  ipc_lite_log_init(config.app.log_level);
  log_config_summary(&config);
  active_config = config;

  install_signal_handlers();

  ipc_lite_file_sink_init(&sinks[0]);
  ipc_lite_rtsp_sink_init(&sinks[1]);
  ipc_lite_rtmp_sink_init(&sinks[2]);
  ipc_lite_udp_rtp_sink_init(&sinks[3]);

  if (config.app.enable_preflight && config_uses_network_sinks(&config)) {
    active_config.rtsp.enable = false;
    active_config.rtmp.enable = false;
    active_config.udp_rtp.enable = false;
    IPC_LITE_LOGI("app",
                  "starting in preflight mode with network sinks disabled");
  }

  if (start_stream_stack(&active_config, &isp, &pipeline, sinks,
                         sizeof(sinks) / sizeof(sinks[0]), &periodic_stats,
                         &last_log_at) != 0) {
    goto cleanup;
  }

  IPC_LITE_LOGI("app", "ipc_lite started in foreground, press Ctrl+C to stop");

  while (!g_stop_requested) {
    struct timespec now;

    usleep(200000);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (elapsed_seconds(&last_log_at, &now) >= config.app.stats_interval_sec) {
      sample_pipeline_stats(&pipeline, &periodic_stats, &stats_sample);
      log_periodic_stats(&config, &stats_sample);

      if (config.app.enable_health_restart &&
          sample_requires_restart(&stats_sample)) {
        health_state.restart_count++;
        IPC_LITE_LOGW("app",
                      "detected unhealthy stream state "
                      "(fps=%.2f kbps=%.2f last_len=%u frame_delta=%llu "
                      "timeout_delta=%llu), restarting stream stack "
                      "(count=%u)",
                      stats_sample.fps, stats_sample.kbps,
                      stats_sample.snapshot.last_len,
                      (unsigned long long)stats_sample.frame_delta,
                      (unsigned long long)stats_sample.timeout_delta,
                      health_state.restart_count);

        stop_stream_stack(&isp, &pipeline);
        usleep(200000);
        health_state.preflight_healthy_intervals = 0;
        active_config = config;
        if (config.app.enable_preflight && config_uses_network_sinks(&config)) {
          active_config.rtsp.enable = false;
          active_config.rtmp.enable = false;
          active_config.udp_rtp.enable = false;
          IPC_LITE_LOGI("app",
                        "retrying in preflight mode with network sinks "
                        "disabled");
        }
        if (start_stream_stack(&active_config, &isp, &pipeline, sinks,
                               sizeof(sinks) / sizeof(sinks[0]),
                               &periodic_stats, &last_log_at) != 0) {
          goto cleanup;
        }
        continue;
      }

      if (!active_config.rtsp.enable && !active_config.rtmp.enable &&
          !active_config.udp_rtp.enable && config.app.enable_preflight &&
          config_uses_network_sinks(&config)) {
        health_state.preflight_healthy_intervals++;
        if (health_state.preflight_healthy_intervals < 2U) {
          IPC_LITE_LOGI("app",
                        "preflight healthy window %u/2, keeping network "
                        "sinks disabled",
                        health_state.preflight_healthy_intervals);
        } else {
          active_config = config;
          IPC_LITE_LOGI("app", "preflight passed, enabling network sinks");
          if (ipc_lite_pipeline_open_disabled_sinks(&pipeline,
                                                    &active_config) != 0) {
            goto cleanup;
          }
          if (config.video.request_idr_on_rtsp_enable) {
            ipc_lite_pipeline_request_idr(&pipeline, false);
          }
        }
        commit_periodic_stats(&periodic_stats, &stats_sample);
        last_log_at = stats_sample.timestamp;
        continue;
      }

      commit_periodic_stats(&periodic_stats, &stats_sample);
      last_log_at = stats_sample.timestamp;
    }
  }

  IPC_LITE_LOGI("app", "stop requested, shutting down");
  ret = 0;

cleanup:
  stop_stream_stack(&isp, &pipeline);
  return ret;
}
