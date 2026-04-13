#include "rtmp_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

typedef struct {
  char url[IPC_LITE_PATH_MAX];
  bool warned;
} IPC_LITE_RTMP_SINK_CONTEXT;

static int rtmp_sink_open(IPC_LITE_STREAM_SINK *sink,
                          const IPC_LITE_CONFIG *config,
                          IPC_LITE_CODEC codec) {
  IPC_LITE_RTMP_SINK_CONTEXT *context = NULL;
  (void)codec;

  if (!config->rtmp.enable) {
    sink->enabled = false;
    return 0;
  }

  context = calloc(1, sizeof(*context));
  if (!context) {
    return -1;
  }

  snprintf(context->url, sizeof(context->url), "%s", config->rtmp.url);
  context->warned = true;
  sink->ctx = context;
  sink->enabled = true;

  IPC_LITE_LOGW("rtmp_sink",
                "rtmp sink is placeholder only, url=%s, packet will be dropped",
                context->url);
  return 0;
}

static int rtmp_sink_write(IPC_LITE_STREAM_SINK *sink,
                           const IPC_LITE_STREAM_PACKET *packet) {
  (void)sink;
  (void)packet;
  return 0;
}

static void rtmp_sink_close(IPC_LITE_STREAM_SINK *sink) {
  free(sink->ctx);
  sink->ctx = NULL;
  sink->enabled = false;
}

void ipc_lite_rtmp_sink_init(IPC_LITE_STREAM_SINK *sink) {
  memset(sink, 0, sizeof(*sink));
  sink->name = "rtmp";
  sink->open = rtmp_sink_open;
  sink->write = rtmp_sink_write;
  sink->close = rtmp_sink_close;
}
