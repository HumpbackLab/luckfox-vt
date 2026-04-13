#include "file_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

typedef struct {
  FILE *fp;
  unsigned int frame_count;
} IPC_LITE_FILE_SINK_CONTEXT;

static int file_sink_open(IPC_LITE_STREAM_SINK *sink,
                          const IPC_LITE_CONFIG *config,
                          IPC_LITE_CODEC codec) {
  IPC_LITE_FILE_SINK_CONTEXT *context = NULL;
  (void)codec;

  if (!config->file_output.enable) {
    sink->enabled = false;
    return 0;
  }

  context = calloc(1, sizeof(*context));
  if (!context) {
    return -1;
  }

  context->fp = fopen(config->file_output.path, "wb");
  if (!context->fp) {
    free(context);
    return -1;
  }

  sink->ctx = context;
  sink->enabled = true;
  IPC_LITE_LOGI("file_sink", "write encoded stream to %s",
                config->file_output.path);
  return 0;
}

static int file_sink_write(IPC_LITE_STREAM_SINK *sink,
                           const IPC_LITE_STREAM_PACKET *packet) {
  IPC_LITE_FILE_SINK_CONTEXT *context = sink->ctx;
  size_t written = 0;

  if (!sink->enabled || !context || !context->fp) {
    return 0;
  }

  written = fwrite(packet->data, 1, packet->len, context->fp);
  if (written != packet->len) {
    return -1;
  }

  context->frame_count++;
  if ((context->frame_count % 30U) == 0U) {
    fflush(context->fp);
  }

  return 0;
}

static void file_sink_close(IPC_LITE_STREAM_SINK *sink) {
  IPC_LITE_FILE_SINK_CONTEXT *context = sink->ctx;

  if (!context) {
    return;
  }

  if (context->fp) {
    fflush(context->fp);
    fclose(context->fp);
  }

  free(context);
  sink->ctx = NULL;
  sink->enabled = false;
}

void ipc_lite_file_sink_init(IPC_LITE_STREAM_SINK *sink) {
  memset(sink, 0, sizeof(*sink));
  sink->name = "file";
  sink->open = file_sink_open;
  sink->write = file_sink_write;
  sink->close = file_sink_close;
}

