#include "rtsp_sink.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "rtsp_demo.h"

typedef struct {
  rtsp_demo_handle demo;
  rtsp_session_handle session;
  time_t last_error_log_at;
  unsigned int error_count;
} IPC_LITE_RTSP_SINK_CONTEXT;

static int rtsp_sink_open(IPC_LITE_STREAM_SINK *sink,
                          const IPC_LITE_CONFIG *config,
                          IPC_LITE_CODEC codec) {
  IPC_LITE_RTSP_SINK_CONTEXT *context = NULL;
  int codec_id = RTSP_CODEC_ID_VIDEO_H264;

  if (!config->rtsp.enable) {
    sink->enabled = false;
    return 0;
  }

  context = calloc(1, sizeof(*context));
  if (!context) {
    return -1;
  }

  if (codec == IPC_LITE_CODEC_H265) {
    codec_id = RTSP_CODEC_ID_VIDEO_H265;
  }

  context->demo = create_rtsp_demo(config->rtsp.port);
  if (!context->demo) {
    free(context);
    return -1;
  }

  context->session = rtsp_new_session(context->demo, config->rtsp.path);
  if (!context->session) {
    rtsp_del_demo(context->demo);
    free(context);
    return -1;
  }

  if (rtsp_set_video(context->session, codec_id, NULL, 0) != 0) {
    rtsp_del_session(context->session);
    rtsp_del_demo(context->demo);
    free(context);
    return -1;
  }

  rtsp_sync_video_ts(context->session, rtsp_get_reltime(), rtsp_get_ntptime());

  sink->ctx = context;
  sink->enabled = true;
  IPC_LITE_LOGI("rtsp_sink", "rtsp listen on rtsp://<board-ip>:%d%s",
                config->rtsp.port, config->rtsp.path);
  return 0;
}

static int rtsp_sink_write(IPC_LITE_STREAM_SINK *sink,
                           const IPC_LITE_STREAM_PACKET *packet) {
  IPC_LITE_RTSP_SINK_CONTEXT *context = sink->ctx;
  int ret = 0;

  if (!sink->enabled || !context) {
    return 0;
  }

  rtsp_do_event(context->demo);
  ret = rtsp_tx_video(context->session, packet->data, (int)packet->len,
                      packet->pts);
  rtsp_do_event(context->demo);
  if (ret != 0) {
    time_t now = time(NULL);

    context->error_count++;
    if (context->last_error_log_at == 0 ||
        difftime(now, context->last_error_log_at) >= 5.0) {
      IPC_LITE_LOGW("rtsp_sink",
                    "rtsp_tx_video returned %d, stream may have no active "
                    "client yet (failures=%u)",
                    ret, context->error_count);
      context->last_error_log_at = now;
    }
  }

  return 0;
}

static void rtsp_sink_close(IPC_LITE_STREAM_SINK *sink) {
  IPC_LITE_RTSP_SINK_CONTEXT *context = sink->ctx;

  if (!context) {
    return;
  }

  if (context->session) {
    rtsp_del_session(context->session);
  }
  if (context->demo) {
    rtsp_del_demo(context->demo);
  }

  free(context);
  sink->ctx = NULL;
  sink->enabled = false;
}

void ipc_lite_rtsp_sink_init(IPC_LITE_STREAM_SINK *sink) {
  memset(sink, 0, sizeof(*sink));
  sink->name = "rtsp";
  sink->open = rtsp_sink_open;
  sink->write = rtsp_sink_write;
  sink->close = rtsp_sink_close;
}
