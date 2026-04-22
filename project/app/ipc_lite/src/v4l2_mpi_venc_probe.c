#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "isp.h"
#include "log.h"
#include "rk_comm_rc.h"
#include "rk_comm_venc.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "stream_sink.h"
#include "udp_rtp_sink.h"

#ifndef VIDEO_MAX_PLANES
#define VIDEO_MAX_PLANES 8
#endif

#define ALIGN_UP(value, alignment) (((value) + ((alignment)-1)) & ~((alignment)-1))

static volatile sig_atomic_t g_stop_requested = 0;

typedef struct {
  int fd;
  MB_BLK mb;
} V4l2Buffer;

typedef struct {
  uint64_t count;
  uint64_t total_us;
  uint64_t max_us;
} TimeStats;

typedef struct {
  const char *config_path;
  const char *dev;
  unsigned int frames;
  unsigned int buffers;
  bool drain_latest;
} ProbeArgs;

typedef struct {
  ProbeArgs args;
  IPC_LITE_CONFIG *config;
  IPC_LITE_STREAM_SINK *sink;
  V4l2Buffer *buffers;
  int fd;
  unsigned int planes_count;
  unsigned int frame_size;
  unsigned int sent_frames;
  unsigned int stream_frames;
  uint64_t bytes;
  uint64_t drained_total;
  uint64_t dropped_streams;
  uint64_t capture_start_us;
  uint64_t capture_elapsed_us;
  bool capture_done;
  int error;
  pthread_mutex_t lock;
  TimeStats age;
  TimeStats wait;
  TimeStats buffer_setup;
  TimeStats buffer_sync;
  TimeStats send_frame;
  TimeStats get_stream;
  TimeStats sink_write;
  TimeStats end_to_end;
} ProbeRuntime;

static const char *fourcc_to_str(RK_U32 fourcc, char out[5]) {
  out[0] = (char)(fourcc & 0xff);
  out[1] = (char)((fourcc >> 8) & 0xff);
  out[2] = (char)((fourcc >> 16) & 0xff);
  out[3] = (char)((fourcc >> 24) & 0xff);
  out[4] = '\0';
  return out;
}

static uint64_t monotonic_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void handle_signal(int signo) {
  (void)signo;
  g_stop_requested = 1;
}

static uint64_t timeval_to_us(const struct timeval *tv) {
  return (uint64_t)tv->tv_sec * 1000000ULL + (uint64_t)tv->tv_usec;
}

static void stats_add(TimeStats *stats, uint64_t value_us) {
  stats->count++;
  stats->total_us += value_us;
  if (value_us > stats->max_us) {
    stats->max_us = value_us;
  }
}

static double stats_avg_ms(const TimeStats *stats) {
  return stats->count ? (double)stats->total_us / (double)stats->count / 1000.0
                      : 0.0;
}

static uint64_t mpi_frame_size(const IPC_LITE_CONFIG *config) {
  uint64_t width = ALIGN_UP((uint64_t)config->video.width, 16U);
  uint64_t height = ALIGN_UP((uint64_t)config->video.height, 16U);

  return width * height * 3 / 2;
}

static void force_sensor_mode(const IPC_LITE_CONFIG *config) {
  char command[256];
  int ret = 0;
  unsigned int width = (unsigned int)config->video.width;
  unsigned int height = (unsigned int)config->video.height;

  snprintf(command, sizeof(command),
           "media-ctl -d /dev/media0 --set-v4l2 "
           "\"'m00_b_mis5001 4-0031':0[fmt:SGRBG10_1X10/%dx%d]\" "
           ">/dev/null 2>&1",
           width, height);
  ret = system(command);
  if (ret != 0) {
    IPC_LITE_LOGW("v4l2_mpi", "failed to force sensor mode %dx%d",
                  width, height);
  }
}

static int xioctl(int fd, unsigned long request, void *arg) {
  int ret = 0;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && errno == EINTR);
  return ret;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [-c config.ini|config.ini] [-n frames] [--drain]\n"
          "Advanced: [-d /dev/videoX] [-b buffers]\n"
          "Defaults: -c ./ipc_lite.ini -d /dev/video12 -n 300 -b 2 format=NV12\n"
          "-n 0 means run until SIGINT/SIGTERM.\n",
          prog);
}

static int parse_args(int argc, char **argv, ProbeArgs *args) {
  int i = 0;

  args->config_path = "./ipc_lite.ini";
  args->dev = "/dev/video12";
  args->frames = 300;
  args->buffers = 2;
  args->drain_latest = false;

  for (i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      args->config_path = argv[i];
    } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
      args->config_path = argv[++i];
    } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
      args->dev = argv[++i];
    } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
      args->frames = (unsigned int)strtoul(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
      args->buffers = (unsigned int)strtoul(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--drain")) {
      args->drain_latest = true;
    } else if (!strcmp(argv[i], "--help")) {
      usage(argv[0]);
      return 1;
    } else {
      usage(argv[0]);
      return -1;
    }
  }

  if (args->buffers < 2) {
    args->buffers = 2;
  }
  if (args->buffers > 8) {
    args->buffers = 8;
  }
  return 0;
}

static int wait_for_v4l2_frame(int fd) {
  struct pollfd pfd;
  int ret = 0;

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = POLLIN | POLLPRI;
  do {
    ret = poll(&pfd, 1, 1000);
  } while (ret == -1 && errno == EINTR);

  if (ret <= 0) {
    fprintf(stderr, "%s\n", ret == 0 ? "v4l2 poll timeout" : strerror(errno));
    return -1;
  }
  return 0;
}

static int queue_v4l2_buffer(int fd, unsigned int index,
                             unsigned int planes_count, int dma_fd,
                             unsigned int sizeimage) {
  struct v4l2_buffer buf;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];

  memset(&buf, 0, sizeof(buf));
  memset(planes, 0, sizeof(planes));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_DMABUF;
  buf.index = index;
  buf.m.planes = planes;
  buf.length = planes_count;
  planes[0].m.fd = dma_fd;
  planes[0].length = sizeimage;

  if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
    fprintf(stderr, "VIDIOC_QBUF(%u) failed: %s\n", index, strerror(errno));
    return -1;
  }
  return 0;
}

static int dequeue_v4l2_buffer(int fd, unsigned int planes_count,
                               struct v4l2_buffer *buf,
                               struct v4l2_plane planes[VIDEO_MAX_PLANES]) {
  memset(buf, 0, sizeof(*buf));
  memset(planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
  buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf->memory = V4L2_MEMORY_DMABUF;
  buf->m.planes = planes;
  buf->length = planes_count;
  return xioctl(fd, VIDIOC_DQBUF, buf);
}

static bool h264_packet_is_key(const VENC_PACK_S *pack, const uint8_t *data,
                               size_t len) {
  size_t i = 0;

  if (pack->DataType.enH264EType == H264E_NALU_IDRSLICE) {
    return true;
  }

  for (i = 0; i + 5 < len; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 &&
        ((data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5) ||
         (data[i + 2] == 0 && data[i + 3] == 1 &&
          (data[i + 4] & 0x1f) == 5))) {
      return true;
    }
  }
  return false;
}

static void fill_h264_rc(VENC_CHN_ATTR_S *attr, const IPC_LITE_CONFIG *config) {
  attr->stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
  attr->stRcAttr.stH264Cbr.u32Gop = (RK_U32)config->video.gop;
  attr->stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)config->video.fps;
  attr->stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
  attr->stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)config->video.fps;
  attr->stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
  attr->stRcAttr.stH264Cbr.u32BitRate = (RK_U32)config->video.bitrate_kbps;
  attr->stRcAttr.stH264Cbr.u32StatTime = 1;
}

static int venc_init(const IPC_LITE_CONFIG *config) {
  VENC_CHN_ATTR_S attr;
  VENC_CHN_PARAM_S chn_param;
  VENC_RECV_PIC_PARAM_S recv_param;
  RK_U32 frame_size =
      (RK_U32)(ALIGN_UP((unsigned int)config->video.width, 16U) *
               ALIGN_UP((unsigned int)config->video.height, 16U) * 3U / 2U);
  int ret = 0;

  memset(&attr, 0, sizeof(attr));
  fill_h264_rc(&attr, config);
  attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
  attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  attr.stVencAttr.u32MaxPicWidth = (RK_U32)config->video.max_width;
  attr.stVencAttr.u32MaxPicHeight = (RK_U32)config->video.max_height;
  attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  attr.stVencAttr.enMirror = MIRROR_NONE;
  attr.stVencAttr.u32BufSize = frame_size;
  attr.stVencAttr.bByFrame = RK_TRUE;
  attr.stVencAttr.u32PicWidth = (RK_U32)config->video.width;
  attr.stVencAttr.u32PicHeight = (RK_U32)config->video.height;
  attr.stVencAttr.u32VirWidth = ALIGN_UP((RK_U32)config->video.width, 16U);
  attr.stVencAttr.u32VirHeight = ALIGN_UP((RK_U32)config->video.height, 16U);
  attr.stVencAttr.u32StreamBufCnt = (RK_U32)config->video.venc_buffer_count;

  ret = RK_MPI_VENC_CreateChn(config->video.venc_channel, &attr);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("v4l2_mpi", "RK_MPI_VENC_CreateChn failed %#x", ret);
    return -1;
  }

  if (config->video.scene_mode != IPC_LITE_SCENE_MODE_DISABLED) {
    RK_MPI_VENC_SetSceneMode(config->video.venc_channel,
                             (VENC_SCENE_MODE_E)config->video.scene_mode);
  }

  memset(&chn_param, 0, sizeof(chn_param));
  if (RK_MPI_VENC_GetChnParam(config->video.venc_channel, &chn_param) ==
      RK_SUCCESS) {
    chn_param.u32MaxStrmCnt = (RK_U32)config->video.max_stream_count;
    chn_param.u32PollWakeUpFrmCnt =
        (RK_U32)config->video.poll_wakeup_frame_count;
    RK_MPI_VENC_SetChnParam(config->video.venc_channel, &chn_param);
  }

  memset(&recv_param, 0, sizeof(recv_param));
  recv_param.s32RecvPicNum = -1;
  ret = RK_MPI_VENC_StartRecvFrame(config->video.venc_channel, &recv_param);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("v4l2_mpi", "RK_MPI_VENC_StartRecvFrame failed %#x", ret);
    RK_MPI_VENC_DestroyChn(config->video.venc_channel);
    return -1;
  }

  if (config->video.request_idr_on_start) {
    RK_MPI_VENC_RequestIDR(config->video.venc_channel, RK_TRUE);
  }

  return 0;
}

static void runtime_set_done(ProbeRuntime *rt, int error) {
  pthread_mutex_lock(&rt->lock);
  rt->capture_done = true;
  if (error != 0 && rt->error == 0) {
    rt->error = error;
  }
  pthread_mutex_unlock(&rt->lock);
}

static void *capture_thread_main(void *arg) {
  ProbeRuntime *rt = (ProbeRuntime *)arg;
  IPC_LITE_CONFIG *config = rt->config;

  while (!g_stop_requested &&
         (rt->args.frames == 0 || rt->sent_frames < rt->args.frames)) {
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    VIDEO_FRAME_INFO_S frame;
    uint64_t poll_start_us = monotonic_us();
    uint64_t dq_done_us = 0;
    uint64_t ts_us = 0;
    unsigned int drained = 0;
    int ret = 0;

    if (wait_for_v4l2_frame(rt->fd) != 0) {
      continue;
    }

    if (dequeue_v4l2_buffer(rt->fd, rt->planes_count, &vbuf, planes) == -1) {
      if (errno == EAGAIN) {
        continue;
      }
      fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
      runtime_set_done(rt, -1);
      return NULL;
    }
    dq_done_us = monotonic_us();

    if (rt->args.drain_latest) {
      struct v4l2_buffer latest = vbuf;
      struct v4l2_plane latest_planes[VIDEO_MAX_PLANES];
      while (dequeue_v4l2_buffer(rt->fd, rt->planes_count, &latest,
                                 latest_planes) == 0) {
        queue_v4l2_buffer(rt->fd, vbuf.index, rt->planes_count,
                          rt->buffers[vbuf.index].fd, rt->frame_size);
        vbuf = latest;
        memcpy(planes, latest_planes, sizeof(planes));
        drained++;
      }
      dq_done_us = monotonic_us();
    }

    ts_us = timeval_to_us(&vbuf.timestamp);

    pthread_mutex_lock(&rt->lock);
    stats_add(&rt->wait, dq_done_us - poll_start_us);
    if (ts_us != 0 && dq_done_us >= ts_us &&
        dq_done_us - ts_us < 10000000ULL) {
      stats_add(&rt->age, dq_done_us - ts_us);
    }
    rt->drained_total += drained;
    pthread_mutex_unlock(&rt->lock);

    {
      uint64_t sync_start_us = monotonic_us();
      ret = RK_MPI_SYS_MmzFlushCache(rt->buffers[vbuf.index].mb, RK_TRUE);
      pthread_mutex_lock(&rt->lock);
      stats_add(&rt->buffer_sync, monotonic_us() - sync_start_us);
      pthread_mutex_unlock(&rt->lock);
      if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_MmzFlushCache failed %#x\n", ret);
        queue_v4l2_buffer(rt->fd, vbuf.index, rt->planes_count,
                          rt->buffers[vbuf.index].fd, rt->frame_size);
        runtime_set_done(rt, -1);
        return NULL;
      }
    }

    memset(&frame, 0, sizeof(frame));
    {
      uint8_t *base =
          (uint8_t *)RK_MPI_MB_Handle2VirAddr(rt->buffers[vbuf.index].mb);
      unsigned int stride = ALIGN_UP((RK_U32)config->video.width, 16U);
      frame.stVFrame.pVirAddr[0] = base;
      frame.stVFrame.pVirAddr[1] =
          base ? base + stride * config->video.height : NULL;
    }
    frame.stVFrame.pMbBlk = rt->buffers[vbuf.index].mb;
    frame.stVFrame.u32Width = (RK_U32)config->video.width;
    frame.stVFrame.u32Height = (RK_U32)config->video.height;
    frame.stVFrame.u32VirWidth = ALIGN_UP((RK_U32)config->video.width, 16U);
    frame.stVFrame.u32VirHeight = ALIGN_UP((RK_U32)config->video.height, 16U);
    frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    frame.stVFrame.u64PTS = ts_us ? ts_us : dq_done_us;

    {
      uint64_t send_start_us = monotonic_us();
      ret = RK_MPI_VENC_SendFrame(config->video.venc_channel, &frame,
                                  config->video.venc_timeout_ms);
      pthread_mutex_lock(&rt->lock);
      stats_add(&rt->send_frame, monotonic_us() - send_start_us);
      pthread_mutex_unlock(&rt->lock);
    }

    queue_v4l2_buffer(rt->fd, vbuf.index, rt->planes_count,
                      rt->buffers[vbuf.index].fd, rt->frame_size);

    if (ret != RK_SUCCESS) {
      fprintf(stderr, "RK_MPI_VENC_SendFrame failed %#x\n", ret);
      runtime_set_done(rt, -1);
      return NULL;
    }

    pthread_mutex_lock(&rt->lock);
    rt->sent_frames++;
    pthread_mutex_unlock(&rt->lock);
  }

  runtime_set_done(rt, 0);
  return NULL;
}

static void *stream_thread_main(void *arg) {
  ProbeRuntime *rt = (ProbeRuntime *)arg;
  IPC_LITE_CONFIG *config = rt->config;
  VENC_STREAM_S stream;
  VENC_STREAM_S next_stream;
  unsigned int idle_after_done = 0;

  memset(&stream, 0, sizeof(stream));
  memset(&next_stream, 0, sizeof(next_stream));
  stream.pstPack = calloc(1, sizeof(*stream.pstPack));
  next_stream.pstPack = calloc(1, sizeof(*next_stream.pstPack));
  if (!stream.pstPack || !next_stream.pstPack) {
    free(stream.pstPack);
    free(next_stream.pstPack);
    runtime_set_done(rt, -1);
    return NULL;
  }

  while (!g_stop_requested) {
    IPC_LITE_STREAM_PACKET packet;
    uint64_t get_start_us = monotonic_us();
    uint64_t get_elapsed_us = 0;
    uint64_t sink_elapsed_us = 0;
    uint64_t total_us = 0;
    uint64_t packet_pts = 0;
    unsigned int stream_frame = 0;
    unsigned int sent_frames = 0;
    unsigned int dropped_streams = 0;
    uint64_t dropped_before = 0;
    uint64_t consumed_after_current = 0;
    uint64_t max_drain = 0;
    bool capture_done = false;
    bool key_frame = false;
    RK_U32 len = 0;
    int ret = 0;

    ret = RK_MPI_VENC_GetStream(config->video.venc_channel, &stream,
                                config->video.venc_timeout_ms);
    get_elapsed_us = monotonic_us() - get_start_us;
    pthread_mutex_lock(&rt->lock);
    stats_add(&rt->get_stream, get_elapsed_us);
    capture_done = rt->capture_done;
    sent_frames = rt->sent_frames;
    stream_frame = rt->stream_frames;
    dropped_before = rt->dropped_streams;
    pthread_mutex_unlock(&rt->lock);

    if (ret != RK_SUCCESS) {
      if (capture_done && (++idle_after_done >= 3 ||
                           stream_frame >= sent_frames)) {
        break;
      }
      continue;
    }
    idle_after_done = 0;

    consumed_after_current = (uint64_t)stream_frame + dropped_before + 1ULL;
    if (rt->sink->enabled) {
      uint64_t normal_inflight =
          config->video.venc_buffer_count > 1
              ? (uint64_t)config->video.venc_buffer_count
              : 2ULL;
      if ((uint64_t)sent_frames > consumed_after_current + normal_inflight) {
        max_drain =
            (uint64_t)sent_frames - consumed_after_current - normal_inflight;
      }
    }

    while (!g_stop_requested && dropped_streams < max_drain) {
      VENC_PACK_S *released_pack = NULL;

      memset(next_stream.pstPack, 0, sizeof(*next_stream.pstPack));
      ret = RK_MPI_VENC_GetStream(config->video.venc_channel, &next_stream, 0);
      if (ret != RK_SUCCESS) {
        break;
      }

      RK_MPI_VENC_ReleaseStream(config->video.venc_channel, &stream);
      released_pack = stream.pstPack;
      stream = next_stream;
      next_stream.pstPack = released_pack;
      dropped_streams++;
    }

    memset(&packet, 0, sizeof(packet));
    packet.data =
        (const uint8_t *)RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
    packet.len = stream.pstPack->u32Len;
    packet.pts = stream.pstPack->u64PTS;
    packet.key_frame =
        h264_packet_is_key(stream.pstPack, packet.data, packet.len);

    {
      uint64_t sink_start_us = monotonic_us();
      if (rt->sink->write) {
        rt->sink->write(rt->sink, &packet);
      }
      sink_elapsed_us = monotonic_us() - sink_start_us;
    }

    packet_pts = packet.pts;
    len = stream.pstPack->u32Len;
    key_frame = packet.key_frame;
    if (packet_pts != 0) {
      total_us = monotonic_us() - packet_pts;
    }

    RK_MPI_VENC_ReleaseStream(config->video.venc_channel, &stream);

    pthread_mutex_lock(&rt->lock);
    stats_add(&rt->sink_write, sink_elapsed_us);
    if (packet_pts != 0) {
      stats_add(&rt->end_to_end, total_us);
    }
    rt->bytes += len;
    rt->dropped_streams += dropped_streams;
    rt->stream_frames++;
    stream_frame = rt->stream_frames;
    sent_frames = rt->sent_frames;
    pthread_mutex_unlock(&rt->lock);

    if (stream_frame <= 5 || stream_frame % 60 == 0) {
      printf("stream=%u sent=%u pts_age=%.3fms get=%.3fms sink=%.3fms "
             "len=%u key=%d dropped=%u\n",
             stream_frame, sent_frames,
             packet_pts ? (double)total_us / 1000.0 : 0.0,
             (double)get_elapsed_us / 1000.0,
             (double)sink_elapsed_us / 1000.0, len, key_frame ? 1 : 0,
             dropped_streams);
    }
  }

  free(stream.pstPack);
  free(next_stream.pstPack);
  return NULL;
}

int main(int argc, char **argv) {
  ProbeArgs args;
  IPC_LITE_CONFIG config;
  IPC_LITE_ISP_CONTEXT isp;
  IPC_LITE_STREAM_SINK sink;
  ProbeRuntime runtime;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  V4l2Buffer buffers[8];
  pthread_t capture_thread;
  pthread_t stream_thread;
  unsigned int planes_count = 1;
  unsigned int i = 0;
  int fd = -1;
  int ret = 0;
  uint64_t frame_size = 0;
  TimeStats buffer_setup = {0};

  ret = parse_args(argc, argv, &args);
  if (ret > 0) {
    return 0;
  }
  if (ret < 0) {
    return 2;
  }

  ipc_lite_config_set_defaults(&config);
  if (ipc_lite_config_load_file(&config, args.config_path) != 0) {
    fprintf(stderr, "failed to load config: %s\n", args.config_path);
    return 1;
  }
  if (config.video.codec != IPC_LITE_CODEC_H264) {
    fprintf(stderr, "v4l2_mpi_venc_probe currently supports h264 only\n");
    return 1;
  }
  force_sensor_mode(&config);
  memset(&isp, 0, sizeof(isp));
  if (ipc_lite_isp_start(&isp, &config) != 0) {
    fprintf(stderr, "failed to start isp\n");
    return 1;
  }

  memset(buffers, 0, sizeof(buffers));
  for (i = 0; i < 8; ++i) {
    buffers[i].fd = -1;
  }
  memset(&sink, 0, sizeof(sink));
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  fd = open(args.dev, O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) {
    fprintf(stderr, "open %s failed: %s\n", args.dev, strerror(errno));
    return 1;
  }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = (unsigned int)config.video.width;
  fmt.fmt.pix_mp.height = (unsigned int)config.video.height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  fmt.fmt.pix_mp.num_planes = 1;
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
    fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }
  planes_count = fmt.fmt.pix_mp.num_planes ? fmt.fmt.pix_mp.num_planes : 1;

  memset(&req, 0, sizeof(req));
  req.count = args.buffers;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_DMABUF;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1 || req.count < 2) {
    fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  if (RK_MPI_SYS_Init() != RK_SUCCESS) {
    fprintf(stderr, "RK_MPI_SYS_Init failed\n");
    close(fd);
    return 1;
  }
  if (venc_init(&config) != 0) {
    RK_MPI_SYS_Exit();
    close(fd);
    return 1;
  }

  frame_size = (uint64_t)fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
  frame_size = frame_size > mpi_frame_size(&config) ? frame_size
                                                    : mpi_frame_size(&config);
  for (i = 0; i < req.count; ++i) {
    uint64_t setup_start_us = monotonic_us();

    if (RK_MPI_MMZ_Alloc(&buffers[i].mb, (RK_U32)frame_size,
                         RK_MMZ_ALLOC_UNCACHEABLE) != RK_SUCCESS) {
      fprintf(stderr, "RK_MPI_MMZ_Alloc buffer %u failed\n", i);
      RK_MPI_VENC_DestroyChn(config.video.venc_channel);
      RK_MPI_SYS_Exit();
      close(fd);
      return 1;
    }
    RK_MPI_MB_SetBufferStride(buffers[i].mb,
                              ALIGN_UP((RK_U32)config.video.width, 16U),
                              ALIGN_UP((RK_U32)config.video.height, 16U));
    buffers[i].fd = RK_MPI_MB_Handle2Fd(buffers[i].mb);
    if (buffers[i].fd < 0) {
      fprintf(stderr, "RK_MPI_MB_Handle2Fd buffer %u failed\n", i);
      RK_MPI_VENC_DestroyChn(config.video.venc_channel);
      RK_MPI_SYS_Exit();
      close(fd);
      return 1;
    }
    stats_add(&buffer_setup, monotonic_us() - setup_start_us);
    printf("dmabuf buffer index=%u fd=%d mb=%p setup=%.3fms\n", i,
           buffers[i].fd, buffers[i].mb,
           (double)buffer_setup.max_us / 1000.0);
  }

  ipc_lite_udp_rtp_sink_init(&sink);
  if (sink.open(&sink, &config, config.video.codec) != 0) {
    fprintf(stderr, "udp_rtp sink open failed\n");
    RK_MPI_VENC_DestroyChn(config.video.venc_channel);
    RK_MPI_SYS_Exit();
    close(fd);
    return 1;
  }

  for (i = 0; i < req.count; ++i) {
    if (queue_v4l2_buffer(fd, i, planes_count, buffers[i].fd,
                          (unsigned int)frame_size) != 0) {
      return 1;
    }
  }
  {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
      fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
      return 1;
    }
  }

  {
    char negotiated_fourcc[5];
    printf("v4l2_mpi_venc_probe dev=%s %dx%d fps=%d frames=%u buffers=%u "
           "drain=%s mode=mpi-dmabuf threaded=yes format=NV12 negotiated=%s "
           "sensor=%ux%u bytesperline=%u frame_size=%llu\n",
           args.dev, config.video.width, config.video.height, config.video.fps,
           args.frames, req.count, args.drain_latest ? "on" : "off",
           fourcc_to_str(fmt.fmt.pix_mp.pixelformat, negotiated_fourcc),
           (unsigned int)config.video.width, (unsigned int)config.video.height,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
           (unsigned long long)frame_size);
  }

  memset(&runtime, 0, sizeof(runtime));
  runtime.args = args;
  runtime.config = &config;
  runtime.sink = &sink;
  runtime.buffers = buffers;
  runtime.fd = fd;
  runtime.planes_count = planes_count;
  runtime.frame_size = (unsigned int)frame_size;
  runtime.buffer_setup = buffer_setup;
  runtime.capture_start_us = monotonic_us();
  ret = pthread_mutex_init(&runtime.lock, NULL);
  if (ret != 0) {
    fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(ret));
    return 1;
  }

  ret = pthread_create(&stream_thread, NULL, stream_thread_main, &runtime);
  if (ret != 0) {
    fprintf(stderr, "pthread_create stream failed: %s\n", strerror(ret));
    return 1;
  }
  ret = pthread_create(&capture_thread, NULL, capture_thread_main, &runtime);
  if (ret != 0) {
    fprintf(stderr, "pthread_create capture failed: %s\n", strerror(ret));
    g_stop_requested = 1;
    pthread_join(stream_thread, NULL);
    return 1;
  }

  pthread_join(capture_thread, NULL);
  runtime.capture_elapsed_us = monotonic_us() - runtime.capture_start_us;
  pthread_join(stream_thread, NULL);

  printf("summary sent=%u streams=%u bytes=%llu drained=%llu "
         "stream_dropped=%llu mode=mpi-dmabuf "
         "threaded=yes elapsed=%.3fs capture_fps=%.2f stream_fps=%.2f "
         "age=%.3f/%.3fms wait=%.3f/%.3fms "
         "buffer_setup=%.3f/%.3fms sync=%.3f/%.3fms send=%.3f/%.3fms "
         "get=%.3f/%.3fms sink=%.3f/%.3fms total_since_v4l2_ts=%.3f/%.3fms\n",
         runtime.sent_frames, runtime.stream_frames,
         (unsigned long long)runtime.bytes,
         (unsigned long long)runtime.drained_total,
         (unsigned long long)runtime.dropped_streams,
         (double)runtime.capture_elapsed_us / 1000000.0,
         runtime.capture_elapsed_us ? (double)runtime.sent_frames * 1000000.0 /
                                          (double)runtime.capture_elapsed_us
                                    : 0.0,
         runtime.capture_elapsed_us ? (double)runtime.stream_frames * 1000000.0 /
                                          (double)runtime.capture_elapsed_us
                                    : 0.0,
         stats_avg_ms(&runtime.age), (double)runtime.age.max_us / 1000.0,
         stats_avg_ms(&runtime.wait), (double)runtime.wait.max_us / 1000.0,
         stats_avg_ms(&runtime.buffer_setup),
         (double)runtime.buffer_setup.max_us / 1000.0,
         stats_avg_ms(&runtime.buffer_sync),
         (double)runtime.buffer_sync.max_us / 1000.0,
         stats_avg_ms(&runtime.send_frame),
         (double)runtime.send_frame.max_us / 1000.0,
         stats_avg_ms(&runtime.get_stream),
         (double)runtime.get_stream.max_us / 1000.0,
         stats_avg_ms(&runtime.sink_write),
         (double)runtime.sink_write.max_us / 1000.0,
         stats_avg_ms(&runtime.end_to_end),
         (double)runtime.end_to_end.max_us / 1000.0);

  ret = runtime.error != 0 ? 1 : 0;
  pthread_mutex_destroy(&runtime.lock);

  if (sink.close) {
    sink.close(&sink);
  }
  RK_MPI_VENC_StopRecvFrame(config.video.venc_channel);
  RK_MPI_VENC_DestroyChn(config.video.venc_channel);
  {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
  }
  for (i = 0; i < req.count; ++i) {
    if (buffers[i].mb != RK_NULL) {
      RK_MPI_MMZ_Free(buffers[i].mb);
      buffers[i].mb = RK_NULL;
    }
  }
  RK_MPI_SYS_Exit();
  ipc_lite_isp_stop(&isp);
  for (i = 0; i < req.count; ++i) {
    if (buffers[i].fd >= 0) {
      close(buffers[i].fd);
    }
  }
  close(fd);
  return ret;
}
