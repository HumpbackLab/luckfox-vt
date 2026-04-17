#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
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
  const char *pixfmt_name;
  RK_U32 v4l2_pixfmt;
  PIXEL_FORMAT_E mpi_pixfmt;
  unsigned int frames;
  unsigned int buffers;
  bool drain_latest;
} ProbeArgs;

static const char *fourcc_to_str(RK_U32 fourcc, char out[5]) {
  out[0] = (char)(fourcc & 0xff);
  out[1] = (char)((fourcc >> 8) & 0xff);
  out[2] = (char)((fourcc >> 16) & 0xff);
  out[3] = (char)((fourcc >> 24) & 0xff);
  out[4] = '\0';
  return out;
}

static int set_pixfmt(ProbeArgs *args, const char *name) {
  if (!strcmp(name, "nv12")) {
    args->pixfmt_name = "nv12";
    args->v4l2_pixfmt = V4L2_PIX_FMT_NV12;
    args->mpi_pixfmt = RK_FMT_YUV420SP;
    return 0;
  }
  if (!strcmp(name, "nv21")) {
    args->pixfmt_name = "nv21";
    args->v4l2_pixfmt = V4L2_PIX_FMT_NV21;
    args->mpi_pixfmt = RK_FMT_YUV420SP_VU;
    return 0;
  }
  fprintf(stderr, "unsupported pixfmt: %s (supported: nv12, nv21)\n", name);
  return -1;
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

static int xioctl(int fd, unsigned long request, void *arg) {
  int ret = 0;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && errno == EINTR);
  return ret;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [-c config.ini] [-d /dev/videoX] [-n frames] [-b buffers] "
          "[--pixfmt nv12|nv21] [--drain]\n"
          "Defaults: -c ./ipc_lite.ini -d /dev/video12 -n 300 -b 2 "
          "--pixfmt nv12\n"
          "-n 0 means run until SIGINT/SIGTERM.\n",
          prog);
}

static int parse_args(int argc, char **argv, ProbeArgs *args) {
  int i = 0;

  args->config_path = "./ipc_lite.ini";
  args->dev = "/dev/video12";
  set_pixfmt(args, "nv12");
  args->frames = 300;
  args->buffers = 2;
  args->drain_latest = false;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc) {
      args->config_path = argv[++i];
    } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
      args->dev = argv[++i];
    } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
      args->frames = (unsigned int)strtoul(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
      args->buffers = (unsigned int)strtoul(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--pixfmt") && i + 1 < argc) {
      if (set_pixfmt(args, argv[++i]) != 0) {
        return -1;
      }
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

static int venc_init(const IPC_LITE_CONFIG *config, PIXEL_FORMAT_E pixfmt) {
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
  attr.stVencAttr.enPixelFormat = pixfmt;
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

int main(int argc, char **argv) {
  ProbeArgs args;
  IPC_LITE_CONFIG config;
  IPC_LITE_ISP_CONTEXT isp;
  IPC_LITE_STREAM_SINK sink;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  V4l2Buffer buffers[8];
  VIDEO_FRAME_INFO_S frame;
  VENC_STREAM_S stream;
  unsigned int planes_count = 1;
  unsigned int i = 0;
  unsigned int sent_frames = 0;
  int fd = -1;
  int ret = 0;
  uint64_t frame_size = 0;
  TimeStats age = {0};
  TimeStats wait = {0};
  TimeStats buffer_setup = {0};
  TimeStats buffer_sync = {0};
  TimeStats send_frame = {0};
  TimeStats get_stream = {0};
  TimeStats sink_write = {0};
  TimeStats end_to_end = {0};
  uint64_t bytes = 0;
  uint64_t drained_total = 0;
  uint64_t capture_start_us = 0;
  uint64_t capture_elapsed_us = 0;

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
  fmt.fmt.pix_mp.pixelformat = args.v4l2_pixfmt;
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
  if (venc_init(&config, args.mpi_pixfmt) != 0) {
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
  capture_start_us = monotonic_us();

  memset(&stream, 0, sizeof(stream));
  stream.pstPack = calloc(1, sizeof(*stream.pstPack));
  if (!stream.pstPack) {
    return 1;
  }

  {
    char negotiated_fourcc[5];
    printf("v4l2_mpi_venc_probe dev=%s %dx%d fps=%d frames=%u buffers=%u "
           "drain=%s mode=mpi-dmabuf pixfmt=%s negotiated=%s "
           "bytesperline=%u frame_size=%llu\n",
         args.dev, config.video.width, config.video.height, config.video.fps,
         args.frames, req.count, args.drain_latest ? "on" : "off",
           args.pixfmt_name,
           fourcc_to_str(fmt.fmt.pix_mp.pixelformat, negotiated_fourcc),
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
         (unsigned long long)frame_size);
  }

  while (!g_stop_requested && (args.frames == 0 || sent_frames < args.frames)) {
    struct v4l2_buffer vbuf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    uint64_t poll_start_us = monotonic_us();
    uint64_t dq_done_us = 0;
    uint64_t ts_us = 0;
    uint64_t send_start_us = 0;
    uint64_t get_start_us = 0;
    uint64_t sink_start_us = 0;
    unsigned int drained = 0;
    IPC_LITE_STREAM_PACKET packet;

    if (wait_for_v4l2_frame(fd) != 0) {
      continue;
    }

    if (dequeue_v4l2_buffer(fd, planes_count, &vbuf, planes) == -1) {
      if (errno == EAGAIN) {
        continue;
      }
      fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
      break;
    }
    dq_done_us = monotonic_us();

    if (args.drain_latest) {
      struct v4l2_buffer latest = vbuf;
      struct v4l2_plane latest_planes[VIDEO_MAX_PLANES];
      while (dequeue_v4l2_buffer(fd, planes_count, &latest, latest_planes) ==
             0) {
        queue_v4l2_buffer(fd, vbuf.index, planes_count, buffers[vbuf.index].fd,
                          (unsigned int)frame_size);
        vbuf = latest;
        memcpy(planes, latest_planes, sizeof(planes));
        drained++;
      }
      dq_done_us = monotonic_us();
      drained_total += drained;
    }

    ts_us = timeval_to_us(&vbuf.timestamp);
    stats_add(&wait, dq_done_us - poll_start_us);
    if (ts_us != 0 && dq_done_us >= ts_us && dq_done_us - ts_us < 10000000ULL) {
      stats_add(&age, dq_done_us - ts_us);
    }

    {
      uint64_t sync_start_us = monotonic_us();
      ret = RK_MPI_SYS_MmzFlushCache(buffers[vbuf.index].mb, RK_TRUE);
      stats_add(&buffer_sync, monotonic_us() - sync_start_us);
      if (ret != RK_SUCCESS) {
        fprintf(stderr, "RK_MPI_SYS_MmzFlushCache failed %#x\n", ret);
        queue_v4l2_buffer(fd, vbuf.index, planes_count, buffers[vbuf.index].fd,
                          (unsigned int)frame_size);
        break;
      }
    }

    memset(&frame, 0, sizeof(frame));
    {
      uint8_t *base = (uint8_t *)RK_MPI_MB_Handle2VirAddr(buffers[vbuf.index].mb);
      unsigned int stride = ALIGN_UP((RK_U32)config.video.width, 16U);
      frame.stVFrame.pVirAddr[0] = base;
      frame.stVFrame.pVirAddr[1] = base ? base + stride * config.video.height : NULL;
    }
    frame.stVFrame.pMbBlk = buffers[vbuf.index].mb;
    frame.stVFrame.u32Width = (RK_U32)config.video.width;
    frame.stVFrame.u32Height = (RK_U32)config.video.height;
    frame.stVFrame.u32VirWidth = ALIGN_UP((RK_U32)config.video.width, 16U);
    frame.stVFrame.u32VirHeight = ALIGN_UP((RK_U32)config.video.height, 16U);
    frame.stVFrame.enPixelFormat = args.mpi_pixfmt;
    frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    frame.stVFrame.u64PTS = ts_us ? ts_us : dq_done_us;

    send_start_us = monotonic_us();
    ret = RK_MPI_VENC_SendFrame(config.video.venc_channel, &frame,
                                config.video.venc_timeout_ms);
    stats_add(&send_frame, monotonic_us() - send_start_us);
    if (ret != RK_SUCCESS) {
      fprintf(stderr, "RK_MPI_VENC_SendFrame failed %#x\n", ret);
      queue_v4l2_buffer(fd, vbuf.index, planes_count, buffers[vbuf.index].fd,
                        (unsigned int)frame_size);
      break;
    }

    get_start_us = monotonic_us();
    ret = RK_MPI_VENC_GetStream(config.video.venc_channel, &stream,
                                config.video.venc_timeout_ms);
    stats_add(&get_stream, monotonic_us() - get_start_us);
    if (ret != RK_SUCCESS) {
      fprintf(stderr, "RK_MPI_VENC_GetStream failed %#x\n", ret);
      queue_v4l2_buffer(fd, vbuf.index, planes_count, buffers[vbuf.index].fd,
                        (unsigned int)frame_size);
      continue;
    }

    memset(&packet, 0, sizeof(packet));
    packet.data = (const uint8_t *)RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
    packet.len = stream.pstPack->u32Len;
    packet.pts = stream.pstPack->u64PTS ? stream.pstPack->u64PTS : frame.stVFrame.u64PTS;
    packet.key_frame = h264_packet_is_key(stream.pstPack, packet.data, packet.len);

    sink_start_us = monotonic_us();
    if (sink.write) {
      sink.write(&sink, &packet);
    }
    stats_add(&sink_write, monotonic_us() - sink_start_us);

    bytes += packet.len;
    if (ts_us != 0) {
      stats_add(&end_to_end, monotonic_us() - ts_us);
    }

    RK_MPI_VENC_ReleaseStream(config.video.venc_channel, &stream);
    queue_v4l2_buffer(fd, vbuf.index, planes_count, buffers[vbuf.index].fd,
                      (unsigned int)frame_size);

    sent_frames++;
    if (sent_frames <= 5 || sent_frames % 60 == 0) {
      printf("frame=%u seq=%u age=%.3fms send=%.3fms "
             "sync=%.3fms get=%.3fms sink=%.3fms "
             "total_since_v4l2_ts=%.3fms len=%u key=%d drained=%u\n",
             sent_frames, vbuf.sequence, ts_us ? (double)(dq_done_us - ts_us) / 1000.0 : 0.0,
             (double)send_frame.max_us / 1000.0,
             (double)buffer_sync.max_us / 1000.0,
             (double)get_stream.max_us / 1000.0, (double)sink_write.max_us / 1000.0,
             ts_us ? (double)(monotonic_us() - ts_us) / 1000.0 : 0.0,
             stream.pstPack->u32Len, packet.key_frame ? 1 : 0, drained);
    }
  }
  capture_elapsed_us = monotonic_us() - capture_start_us;

  printf("summary frames=%u bytes=%llu drained=%llu mode=mpi-dmabuf "
         "elapsed=%.3fs fps=%.2f age=%.3f/%.3fms wait=%.3f/%.3fms "
         "buffer_setup=%.3f/%.3fms sync=%.3f/%.3fms send=%.3f/%.3fms "
         "get=%.3f/%.3fms sink=%.3f/%.3fms total_since_v4l2_ts=%.3f/%.3fms\n",
         sent_frames, (unsigned long long)bytes,
         (unsigned long long)drained_total,
         (double)capture_elapsed_us / 1000000.0,
         capture_elapsed_us ? (double)sent_frames * 1000000.0 /
                                  (double)capture_elapsed_us
                            : 0.0,
         stats_avg_ms(&age),
         (double)age.max_us / 1000.0, stats_avg_ms(&wait),
         (double)wait.max_us / 1000.0, stats_avg_ms(&buffer_setup),
         (double)buffer_setup.max_us / 1000.0, stats_avg_ms(&buffer_sync),
         (double)buffer_sync.max_us / 1000.0, stats_avg_ms(&send_frame),
         (double)send_frame.max_us / 1000.0, stats_avg_ms(&get_stream),
         (double)get_stream.max_us / 1000.0, stats_avg_ms(&sink_write),
         (double)sink_write.max_us / 1000.0, stats_avg_ms(&end_to_end),
         (double)end_to_end.max_us / 1000.0);

  if (sink.close) {
    sink.close(&sink);
  }
  free(stream.pstPack);
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
  return 0;
}
