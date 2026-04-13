#include "mpi_pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "rk_comm_rc.h"
#include "rk_comm_venc.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#define IPC_LITE_ALIGN(value, alignment) \
  (((value) + ((alignment)-1)) & ~((alignment)-1))

static bool packet_is_key_frame(IPC_LITE_CODEC codec,
                                const VENC_PACK_S *pack) {
  if (codec == IPC_LITE_CODEC_H264) {
    return pack->DataType.enH264EType == H264E_NALU_IDRSLICE;
  }

  return pack->DataType.enH265EType == H265E_NALU_IDRSLICE;
}

static int vi_dev_init(void) {
  VI_DEV_ATTR_S dev_attr;
  VI_DEV_BIND_PIPE_S bind_pipe;
  int ret = 0;

  memset(&dev_attr, 0, sizeof(dev_attr));
  memset(&bind_pipe, 0, sizeof(bind_pipe));

  ret = RK_MPI_VI_GetDevAttr(0, &dev_attr);
  if (ret == RK_ERR_VI_NOT_CONFIG) {
    ret = RK_MPI_VI_SetDevAttr(0, &dev_attr);
    if (ret != RK_SUCCESS) {
      IPC_LITE_LOGE("pipeline", "RK_MPI_VI_SetDevAttr failed %#x", ret);
      return -1;
    }
  }

  ret = RK_MPI_VI_GetDevIsEnable(0);
  if (ret != RK_SUCCESS) {
    ret = RK_MPI_VI_EnableDev(0);
    if (ret != RK_SUCCESS) {
      IPC_LITE_LOGE("pipeline", "RK_MPI_VI_EnableDev failed %#x", ret);
      return -1;
    }

    bind_pipe.u32Num = 1;
    bind_pipe.PipeId[0] = 0;
    ret = RK_MPI_VI_SetDevBindPipe(0, &bind_pipe);
    if (ret != RK_SUCCESS) {
      IPC_LITE_LOGE("pipeline", "RK_MPI_VI_SetDevBindPipe failed %#x", ret);
      return -1;
    }
  }

  return 0;
}

static int vi_chn_init(const IPC_LITE_CONFIG *config) {
  VI_CHN_ATTR_S chn_attr;
  int ret = 0;

  memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.stSize.u32Width = (RK_U32)config->video.width;
  chn_attr.stSize.u32Height = (RK_U32)config->video.height;
  chn_attr.enPixelFormat = RK_FMT_YUV420SP;
  chn_attr.enCompressMode = COMPRESS_MODE_NONE;
  chn_attr.u32Depth = 0;
  chn_attr.stFrameRate.s32SrcFrameRate = -1;
  chn_attr.stFrameRate.s32DstFrameRate = -1;
  chn_attr.stIspOpt.u32BufCount = 4;
  chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
  chn_attr.stIspOpt.stMaxSize.u32Width = (RK_U32)config->video.width;
  chn_attr.stIspOpt.stMaxSize.u32Height = (RK_U32)config->video.height;

  ret = RK_MPI_VI_SetChnAttr(0, config->video.vi_channel, &chn_attr);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_VI_SetChnAttr failed %#x", ret);
    return -1;
  }

  ret = RK_MPI_VI_EnableChn(0, config->video.vi_channel);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_VI_EnableChn failed %#x", ret);
    return -1;
  }

  return 0;
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

static void fill_h265_rc(VENC_CHN_ATTR_S *attr, const IPC_LITE_CONFIG *config) {
  attr->stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
  attr->stRcAttr.stH265Cbr.u32Gop = (RK_U32)config->video.gop;
  attr->stRcAttr.stH265Cbr.u32SrcFrameRateNum = (RK_U32)config->video.fps;
  attr->stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
  attr->stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)config->video.fps;
  attr->stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
  attr->stRcAttr.stH265Cbr.u32BitRate = (RK_U32)config->video.bitrate_kbps;
  attr->stRcAttr.stH265Cbr.u32StatTime = 1;
}

static int venc_init(const IPC_LITE_CONFIG *config) {
  VENC_CHN_ATTR_S attr;
  VENC_RECV_PIC_PARAM_S recv_param;
  int ret = 0;

  memset(&attr, 0, sizeof(attr));
  memset(&recv_param, 0, sizeof(recv_param));

  if (config->video.codec == IPC_LITE_CODEC_H264) {
    fill_h264_rc(&attr, config);
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  } else {
    fill_h265_rc(&attr, config);
    attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
    attr.stVencAttr.u32Profile = H265E_PROFILE_MAIN;
  }

  attr.stVencAttr.u32MaxPicWidth = (RK_U32)config->video.width;
  attr.stVencAttr.u32MaxPicHeight = (RK_U32)config->video.height;
  attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  attr.stVencAttr.enMirror = MIRROR_NONE;
  attr.stVencAttr.u32BufSize =
      (RK_U32)(config->video.width * config->video.height * 3 / 2);
  attr.stVencAttr.bByFrame = RK_TRUE;
  attr.stVencAttr.u32PicWidth = (RK_U32)config->video.width;
  attr.stVencAttr.u32PicHeight = (RK_U32)config->video.height;
  attr.stVencAttr.u32VirWidth = IPC_LITE_ALIGN((RK_U32)config->video.width, 16);
  attr.stVencAttr.u32VirHeight =
      IPC_LITE_ALIGN((RK_U32)config->video.height, 16);
  attr.stVencAttr.u32StreamBufCnt = 4;

  ret = RK_MPI_VENC_CreateChn(config->video.venc_channel, &attr);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_VENC_CreateChn failed %#x", ret);
    return -1;
  }

  recv_param.s32RecvPicNum = -1;
  ret = RK_MPI_VENC_StartRecvFrame(config->video.venc_channel, &recv_param);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_VENC_StartRecvFrame failed %#x", ret);
    return -1;
  }

  return 0;
}

static void *stream_thread_main(void *arg) {
  IPC_LITE_MPI_PIPELINE *pipeline = arg;
  VENC_STREAM_S stream;

  memset(&stream, 0, sizeof(stream));
  stream.pstPack = calloc(1, sizeof(*stream.pstPack));
  if (!stream.pstPack) {
    IPC_LITE_LOGE("pipeline", "failed to allocate VENC_PACK_S");
    return NULL;
  }

  while (!pipeline->stop_requested) {
    int ret = RK_MPI_VENC_GetStream(pipeline->config->video.venc_channel, &stream,
                                    pipeline->config->video.venc_timeout_ms);

    if (ret == RK_SUCCESS) {
      IPC_LITE_STREAM_PACKET packet;
      uint8_t *base =
          (uint8_t *)RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
      size_t i = 0;

      memset(&packet, 0, sizeof(packet));
      packet.data = base + stream.pstPack->u32Offset;
      packet.len = stream.pstPack->u32Len;
      packet.pts = stream.pstPack->u64PTS;
      packet.key_frame =
          packet_is_key_frame(pipeline->config->video.codec, stream.pstPack);

      pthread_mutex_lock(&pipeline->stats_lock);
      pipeline->frames++;
      pipeline->bytes += packet.len;
      pipeline->last_pts = packet.pts;
      pipeline->last_len = (uint32_t)packet.len;
      pthread_mutex_unlock(&pipeline->stats_lock);

      for (i = 0; i < pipeline->sink_count; ++i) {
        if (pipeline->sinks[i].write &&
            pipeline->sinks[i].write(&pipeline->sinks[i], &packet) != 0) {
          IPC_LITE_LOGW("pipeline", "sink %s write failed",
                        pipeline->sinks[i].name);
        }
      }

      ret = RK_MPI_VENC_ReleaseStream(pipeline->config->video.venc_channel,
                                      &stream);
      if (ret != RK_SUCCESS) {
        IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_ReleaseStream failed %#x", ret);
      }
    } else {
      pthread_mutex_lock(&pipeline->stats_lock);
      if (ret == RK_ERR_VENC_BUF_EMPTY) {
        pipeline->timeouts++;
      } else {
        pipeline->errors++;
      }
      pthread_mutex_unlock(&pipeline->stats_lock);
      usleep(10000);
    }
  }

  free(stream.pstPack);
  return NULL;
}

int ipc_lite_pipeline_start(IPC_LITE_MPI_PIPELINE *pipeline,
                            const IPC_LITE_CONFIG *config,
                            IPC_LITE_STREAM_SINK *sinks, size_t sink_count) {
  MPP_CHN_S src_chn;
  MPP_CHN_S dst_chn;
  size_t i = 0;

  memset(pipeline, 0, sizeof(*pipeline));
  pipeline->config = config;
  pipeline->sinks = sinks;
  pipeline->sink_count = sink_count;
  pthread_mutex_init(&pipeline->stats_lock, NULL);
  pipeline->stats_lock_initialized = true;

  for (i = 0; i < sink_count; ++i) {
    if (sinks[i].open && sinks[i].open(&sinks[i], config, config->video.codec) != 0) {
      IPC_LITE_LOGE("pipeline", "failed to open sink %s", sinks[i].name);
      ipc_lite_pipeline_stop(pipeline);
      return -1;
    }
  }

  if (RK_MPI_SYS_Init() != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_SYS_Init failed");
    ipc_lite_pipeline_stop(pipeline);
    return -1;
  }
  pipeline->sys_inited = true;

  if (vi_dev_init() != 0 || vi_chn_init(config) != 0) {
    ipc_lite_pipeline_stop(pipeline);
    return -1;
  }
  pipeline->vi_enabled = true;

  if (venc_init(config) != 0) {
    ipc_lite_pipeline_stop(pipeline);
    return -1;
  }
  pipeline->venc_enabled = true;

  memset(&src_chn, 0, sizeof(src_chn));
  memset(&dst_chn, 0, sizeof(dst_chn));
  src_chn.enModId = RK_ID_VI;
  src_chn.s32DevId = 0;
  src_chn.s32ChnId = config->video.vi_channel;
  dst_chn.enModId = RK_ID_VENC;
  dst_chn.s32DevId = 0;
  dst_chn.s32ChnId = config->video.venc_channel;

  if (RK_MPI_SYS_Bind(&src_chn, &dst_chn) != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_SYS_Bind failed");
    ipc_lite_pipeline_stop(pipeline);
    return -1;
  }
  pipeline->bound = true;

  if (pthread_create(&pipeline->stream_thread, NULL, stream_thread_main,
                     pipeline) != 0) {
    IPC_LITE_LOGE("pipeline", "pthread_create failed");
    ipc_lite_pipeline_stop(pipeline);
    return -1;
  }
  pipeline->stream_thread_started = true;

  return 0;
}

void ipc_lite_pipeline_request_stop(IPC_LITE_MPI_PIPELINE *pipeline) {
  pipeline->stop_requested = true;
}

void ipc_lite_pipeline_stop(IPC_LITE_MPI_PIPELINE *pipeline) {
  size_t i = 0;
  MPP_CHN_S src_chn;
  MPP_CHN_S dst_chn;

  pipeline->stop_requested = true;

  if (pipeline->stream_thread_started) {
    pthread_join(pipeline->stream_thread, NULL);
    pipeline->stream_thread_started = false;
  }

  memset(&src_chn, 0, sizeof(src_chn));
  memset(&dst_chn, 0, sizeof(dst_chn));
  src_chn.enModId = RK_ID_VI;
  src_chn.s32DevId = 0;
  src_chn.s32ChnId = pipeline->config ? pipeline->config->video.vi_channel : 0;
  dst_chn.enModId = RK_ID_VENC;
  dst_chn.s32DevId = 0;
  dst_chn.s32ChnId =
      pipeline->config ? pipeline->config->video.venc_channel : 0;

  if (pipeline->bound) {
    RK_MPI_SYS_UnBind(&src_chn, &dst_chn);
    pipeline->bound = false;
  }

  if (pipeline->venc_enabled) {
    RK_MPI_VENC_StopRecvFrame(pipeline->config->video.venc_channel);
    RK_MPI_VENC_DestroyChn(pipeline->config->video.venc_channel);
    pipeline->venc_enabled = false;
  }

  if (pipeline->vi_enabled) {
    RK_MPI_VI_DisableChn(0, pipeline->config->video.vi_channel);
    RK_MPI_VI_DisableDev(0);
    pipeline->vi_enabled = false;
  }

  if (pipeline->sys_inited) {
    RK_MPI_SYS_Exit();
    pipeline->sys_inited = false;
  }

  for (i = 0; i < pipeline->sink_count; ++i) {
    if (pipeline->sinks[i].close) {
      pipeline->sinks[i].close(&pipeline->sinks[i]);
    }
  }

  if (pipeline->stats_lock_initialized) {
    pthread_mutex_destroy(&pipeline->stats_lock);
    pipeline->stats_lock_initialized = false;
  }
}

void ipc_lite_pipeline_get_stats(IPC_LITE_MPI_PIPELINE *pipeline,
                                 IPC_LITE_PIPELINE_STATS_SNAPSHOT *snapshot) {
  pthread_mutex_lock(&pipeline->stats_lock);
  snapshot->frames = pipeline->frames;
  snapshot->bytes = pipeline->bytes;
  snapshot->timeouts = pipeline->timeouts;
  snapshot->errors = pipeline->errors;
  snapshot->last_pts = pipeline->last_pts;
  snapshot->last_len = pipeline->last_len;
  pthread_mutex_unlock(&pipeline->stats_lock);
}
