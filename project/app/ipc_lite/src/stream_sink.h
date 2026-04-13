#ifndef IPC_LITE_STREAM_SINK_H
#define IPC_LITE_STREAM_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct {
  const uint8_t *data;
  size_t len;
  uint64_t pts;
  bool key_frame;
} IPC_LITE_STREAM_PACKET;

typedef struct IPC_LITE_STREAM_SINK IPC_LITE_STREAM_SINK;

struct IPC_LITE_STREAM_SINK {
  const char *name;
  bool enabled;
  void *ctx;
  int (*open)(IPC_LITE_STREAM_SINK *sink, const IPC_LITE_CONFIG *config,
              IPC_LITE_CODEC codec);
  int (*write)(IPC_LITE_STREAM_SINK *sink,
               const IPC_LITE_STREAM_PACKET *packet);
  void (*close)(IPC_LITE_STREAM_SINK *sink);
};

#endif

