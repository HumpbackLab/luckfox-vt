#ifndef IPC_LITE_MPI_PIPELINE_H
#define IPC_LITE_MPI_PIPELINE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "stream_sink.h"

typedef struct {
  uint64_t frames;
  uint64_t bytes;
  uint64_t timeouts;
  uint64_t errors;
  uint64_t last_pts;
  uint32_t last_len;
} IPC_LITE_PIPELINE_STATS_SNAPSHOT;

typedef struct {
  const IPC_LITE_CONFIG *config;
  IPC_LITE_STREAM_SINK *sinks;
  size_t sink_count;
  pthread_t stream_thread;
  pthread_mutex_t stats_lock;
  bool stats_lock_initialized;
  bool stream_thread_started;
  bool stop_requested;
  bool sys_inited;
  bool vi_enabled;
  bool venc_enabled;
  bool bound;
  uint64_t frames;
  uint64_t bytes;
  uint64_t timeouts;
  uint64_t errors;
  uint64_t last_pts;
  uint32_t last_len;
} IPC_LITE_MPI_PIPELINE;

int ipc_lite_pipeline_start(IPC_LITE_MPI_PIPELINE *pipeline,
                            const IPC_LITE_CONFIG *config,
                            IPC_LITE_STREAM_SINK *sinks, size_t sink_count);
void ipc_lite_pipeline_request_stop(IPC_LITE_MPI_PIPELINE *pipeline);
void ipc_lite_pipeline_stop(IPC_LITE_MPI_PIPELINE *pipeline);
void ipc_lite_pipeline_get_stats(IPC_LITE_MPI_PIPELINE *pipeline,
                                 IPC_LITE_PIPELINE_STATS_SNAPSHOT *snapshot);

#endif
