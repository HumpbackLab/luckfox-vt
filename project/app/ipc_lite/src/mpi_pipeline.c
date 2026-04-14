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
  chn_attr.stFrameRate.s32SrcFrameRate = config->video.fps;
  chn_attr.stFrameRate.s32DstFrameRate = config->video.fps;
  chn_attr.stIspOpt.u32BufCount = (RK_U32)config->video.input_buffer_count;
  chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
  chn_attr.stIspOpt.stMaxSize.u32Width = (RK_U32)config->video.max_width;
  chn_attr.stIspOpt.stMaxSize.u32Height = (RK_U32)config->video.max_height;

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

static RK_U32 effective_venc_buffer_size(const IPC_LITE_CONFIG *config) {
  const RK_U32 configured = (RK_U32)config->video.venc_buffer_size;
  const RK_U32 conservative_min =
      ((RK_U32)config->video.width * (RK_U32)config->video.height * 3U) / 2U;
  const RK_U32 baseline_pixels = 704U * 576U;
  const RK_U32 current_pixels =
      (RK_U32)config->video.width * (RK_U32)config->video.height;

  if (current_pixels > baseline_pixels && configured < conservative_min) {
    IPC_LITE_LOGW("pipeline",
                  "venc_buffer_size=%u too small for %dx%d, bump to %u",
                  configured, config->video.width, config->video.height,
                  conservative_min);
    return conservative_min;
  }

  return configured;
}

static void apply_h264_defaults(int venc_channel) {
  static const RK_U32 thrd_i[16] = {0, 0, 0, 0, 3, 3, 5, 5,
                                    8, 8, 8, 15, 15, 20, 25, 25};
  static const RK_U32 thrd_p[16] = {0, 0, 0, 0, 3, 3, 5, 5,
                                    8, 8, 8, 15, 15, 20, 25, 25};
  static const RK_S32 aq_step_i[16] = {-8, -7, -6, -5, -4, -3, -2, -1,
                                       0,  1,  2,  3,  4,  5,  7,  8};
  static const RK_S32 aq_step_p[16] = {-8, -7, -6, -5, -4, -3, -2, -1,
                                       0,  1,  2,  3,  4,  5,  7,  8};
  VENC_RC_PARAM_S rc_param;
  VENC_RC_PARAM2_S rc_param2;
  VENC_H264_QBIAS_S qbias;
  VENC_H264_TRANS_S trans;
  VENC_FILTER_S filter;
  VENC_ANTI_RING_S anti_ring;
  VENC_ANTI_LINE_S anti_line;
  VENC_LAMBDA_S lambda;

  memset(&rc_param, 0, sizeof(rc_param));
  if (RK_MPI_VENC_GetRcParam(venc_channel, &rc_param) == RK_SUCCESS) {
    rc_param.stParamH264.u32MinQp = 20;
    rc_param.stParamH264.u32FrmMinIQp = 26;
    rc_param.stParamH264.u32FrmMinQp = 28;
    rc_param.stParamH264.u32FrmMaxIQp = 51;
    rc_param.stParamH264.u32FrmMaxQp = 51;
    RK_MPI_VENC_SetRcParam(venc_channel, &rc_param);
  }

  memset(&rc_param2, 0, sizeof(rc_param2));
  if (RK_MPI_VENC_GetRcParam2(venc_channel, &rc_param2) == RK_SUCCESS) {
    memcpy(rc_param2.u32ThrdI, thrd_i, sizeof(thrd_i));
    memcpy(rc_param2.u32ThrdP, thrd_p, sizeof(thrd_p));
    memcpy(rc_param2.s32AqStepI, aq_step_i, sizeof(aq_step_i));
    memcpy(rc_param2.s32AqStepP, aq_step_p, sizeof(aq_step_p));
    RK_MPI_VENC_SetRcParam2(venc_channel, &rc_param2);
  }

  memset(&qbias, 0, sizeof(qbias));
  qbias.bEnable = RK_TRUE;
  qbias.u32QbiasI = 171;
  qbias.u32QbiasP = 85;
  RK_MPI_VENC_SetH264Qbias(venc_channel, &qbias);

  memset(&trans, 0, sizeof(trans));
  if (RK_MPI_VENC_GetH264Trans(venc_channel, &trans) == RK_SUCCESS) {
    trans.bScalingListValid = RK_FALSE;
    RK_MPI_VENC_SetH264Trans(venc_channel, &trans);
  }

  memset(&filter, 0, sizeof(filter));
  if (RK_MPI_VENC_GetFilter(venc_channel, &filter) == RK_SUCCESS) {
    filter.u32StrengthI = 0;
    filter.u32StrengthP = 0;
    RK_MPI_VENC_SetFilter(venc_channel, &filter);
  }

  memset(&anti_ring, 0, sizeof(anti_ring));
  if (RK_MPI_VENC_GetAntiRing(venc_channel, &anti_ring) == RK_SUCCESS) {
    anti_ring.u32AntiRing = 2;
    RK_MPI_VENC_SetAntiRing(venc_channel, &anti_ring);
  }

  memset(&anti_line, 0, sizeof(anti_line));
  if (RK_MPI_VENC_GetAntiLine(venc_channel, &anti_line) == RK_SUCCESS) {
    anti_line.u32AntiLine = 2;
    RK_MPI_VENC_SetAntiLine(venc_channel, &anti_line);
  }

  memset(&lambda, 0, sizeof(lambda));
  if (RK_MPI_VENC_GetLambda(venc_channel, &lambda) == RK_SUCCESS) {
    lambda.u32Lambda = 4;
    RK_MPI_VENC_SetLambda(venc_channel, &lambda);
  }
}

static void apply_h265_defaults(int venc_channel) {
  static const RK_U32 thrd_i[16] = {0, 0, 0, 0, 3, 3, 5, 5,
                                    8, 8, 8, 15, 15, 20, 25, 25};
  static const RK_U32 thrd_p[16] = {0, 0, 0, 0, 3, 3, 5, 5,
                                    8, 8, 8, 15, 15, 20, 25, 25};
  static const RK_S32 aq_step_i[16] = {-8, -7, -6, -5, -4, -3, -2, -1,
                                       0,  1,  2,  3,  4,  5,  7,  8};
  static const RK_S32 aq_step_p[16] = {-8, -7, -6, -5, -4, -3, -2, -1,
                                       0,  1,  2,  3,  4,  5,  7,  8};
  VENC_RC_PARAM_S rc_param;
  VENC_RC_PARAM2_S rc_param2;
  VENC_H265_QBIAS_S qbias;
  VENC_FILTER_S filter;
  VENC_ANTI_RING_S anti_ring;
  VENC_ANTI_LINE_S anti_line;
  VENC_LAMBDA_S lambda;

  memset(&rc_param, 0, sizeof(rc_param));
  if (RK_MPI_VENC_GetRcParam(venc_channel, &rc_param) == RK_SUCCESS) {
    rc_param.stParamH265.u32MinQp = 20;
    rc_param.stParamH265.u32FrmMinIQp = 26;
    rc_param.stParamH265.u32FrmMinQp = 28;
    rc_param.stParamH265.u32FrmMaxIQp = 51;
    rc_param.stParamH265.u32FrmMaxQp = 51;
    RK_MPI_VENC_SetRcParam(venc_channel, &rc_param);
  }

  memset(&rc_param2, 0, sizeof(rc_param2));
  if (RK_MPI_VENC_GetRcParam2(venc_channel, &rc_param2) == RK_SUCCESS) {
    memcpy(rc_param2.u32ThrdI, thrd_i, sizeof(thrd_i));
    memcpy(rc_param2.u32ThrdP, thrd_p, sizeof(thrd_p));
    memcpy(rc_param2.s32AqStepI, aq_step_i, sizeof(aq_step_i));
    memcpy(rc_param2.s32AqStepP, aq_step_p, sizeof(aq_step_p));
    RK_MPI_VENC_SetRcParam2(venc_channel, &rc_param2);
  }

  memset(&qbias, 0, sizeof(qbias));
  qbias.bEnable = RK_TRUE;
  qbias.u32QbiasI = 171;
  qbias.u32QbiasP = 85;
  RK_MPI_VENC_SetH265Qbias(venc_channel, &qbias);

  memset(&filter, 0, sizeof(filter));
  if (RK_MPI_VENC_GetFilter(venc_channel, &filter) == RK_SUCCESS) {
    filter.u32StrengthI = 0;
    filter.u32StrengthP = 0;
    RK_MPI_VENC_SetFilter(venc_channel, &filter);
  }

  memset(&anti_ring, 0, sizeof(anti_ring));
  if (RK_MPI_VENC_GetAntiRing(venc_channel, &anti_ring) == RK_SUCCESS) {
    anti_ring.u32AntiRing = 2;
    RK_MPI_VENC_SetAntiRing(venc_channel, &anti_ring);
  }

  memset(&anti_line, 0, sizeof(anti_line));
  if (RK_MPI_VENC_GetAntiLine(venc_channel, &anti_line) == RK_SUCCESS) {
    anti_line.u32AntiLine = 2;
    RK_MPI_VENC_SetAntiLine(venc_channel, &anti_line);
  }

  memset(&lambda, 0, sizeof(lambda));
  if (RK_MPI_VENC_GetLambda(venc_channel, &lambda) == RK_SUCCESS) {
    lambda.u32Lambda = 4;
    RK_MPI_VENC_SetLambda(venc_channel, &lambda);
  }
}

static void sync_frame_rate(const IPC_LITE_CONFIG *config) {
  VI_CHN_ATTR_S vi_attr;
  VENC_CHN_ATTR_S venc_attr;

  memset(&vi_attr, 0, sizeof(vi_attr));
  if (RK_MPI_VI_GetChnAttr(0, config->video.vi_channel, &vi_attr) ==
      RK_SUCCESS) {
    vi_attr.stFrameRate.s32SrcFrameRate = config->video.fps;
    vi_attr.stFrameRate.s32DstFrameRate = config->video.fps;
    RK_MPI_VI_SetChnAttr(0, config->video.vi_channel, &vi_attr);
  }

  memset(&venc_attr, 0, sizeof(venc_attr));
  if (RK_MPI_VENC_GetChnAttr(config->video.venc_channel, &venc_attr) !=
      RK_SUCCESS) {
    return;
  }

  if (config->video.codec == IPC_LITE_CODEC_H264) {
    venc_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    if (venc_attr.stRcAttr.enRcMode == VENC_RC_MODE_H264CBR) {
      venc_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
      venc_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
          (RK_U32)config->video.fps;
      venc_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
      venc_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
          (RK_U32)config->video.fps;
    }
  } else {
    venc_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
    if (venc_attr.stRcAttr.enRcMode == VENC_RC_MODE_H265CBR) {
      venc_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
      venc_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
          (RK_U32)config->video.fps;
      venc_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
      venc_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
          (RK_U32)config->video.fps;
    }
  }

  RK_MPI_VENC_SetChnAttr(config->video.venc_channel, &venc_attr);
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

static void apply_scene_mode(const IPC_LITE_CONFIG *config) {
  int ret = 0;

  if (config->video.scene_mode == IPC_LITE_SCENE_MODE_DISABLED) {
    return;
  }

  ret = RK_MPI_VENC_SetSceneMode(
      config->video.venc_channel, (VENC_SCENE_MODE_E)config->video.scene_mode);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_SetSceneMode failed %#x", ret);
    return;
  }

  IPC_LITE_LOGI("pipeline", "scene mode set to %d", config->video.scene_mode);
}

static void apply_channel_params(const IPC_LITE_CONFIG *config) {
  VENC_CHN_PARAM_S chn_param;
  int ret = 0;

  if (config->video.max_stream_count <= 0 &&
      config->video.poll_wakeup_frame_count <= 0) {
    return;
  }

  memset(&chn_param, 0, sizeof(chn_param));
  ret = RK_MPI_VENC_GetChnParam(config->video.venc_channel, &chn_param);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_GetChnParam failed %#x", ret);
    return;
  }

  if (config->video.max_stream_count > 0) {
    chn_param.u32MaxStrmCnt = (RK_U32)config->video.max_stream_count;
  }
  if (config->video.poll_wakeup_frame_count > 0) {
    chn_param.u32PollWakeUpFrmCnt =
        (RK_U32)config->video.poll_wakeup_frame_count;
  }

  ret = RK_MPI_VENC_SetChnParam(config->video.venc_channel, &chn_param);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_SetChnParam failed %#x", ret);
    return;
  }

  IPC_LITE_LOGI("pipeline", "stream param max_cnt=%u wakeup=%u",
                chn_param.u32MaxStrmCnt, chn_param.u32PollWakeUpFrmCnt);
}

static void apply_motion_tuning(const IPC_LITE_CONFIG *config) {
  int ret = 0;

  if (config->video.enable_motion_deblur) {
    ret = RK_MPI_VENC_EnableMotionDeblur(config->video.venc_channel, RK_TRUE);
    if (ret != RK_SUCCESS) {
      IPC_LITE_LOGW("pipeline",
                    "RK_MPI_VENC_EnableMotionDeblur failed %#x", ret);
    } else {
      ret = RK_MPI_VENC_SetMotionDeblurStrength(
          config->video.venc_channel,
          (RK_U32)config->video.motion_deblur_strength);
      if (ret != RK_SUCCESS) {
        IPC_LITE_LOGW("pipeline",
                      "RK_MPI_VENC_SetMotionDeblurStrength failed %#x", ret);
      }
    }
  }

  if (config->video.enable_motion_static_switch) {
    ret = RK_MPI_VENC_EnableMotionStaticSwitch(config->video.venc_channel,
                                               RK_TRUE);
    if (ret != RK_SUCCESS) {
      IPC_LITE_LOGW("pipeline",
                    "RK_MPI_VENC_EnableMotionStaticSwitch failed %#x", ret);
    }
  }
}

static void apply_slice_split(const IPC_LITE_CONFIG *config) {
  VENC_SLICE_SPLIT_S slice_split;
  int ret = 0;

  if (!config->video.enable_slice_split || config->video.slice_split_size <= 0) {
    return;
  }

  memset(&slice_split, 0, sizeof(slice_split));
  slice_split.bSplitEnable = RK_TRUE;
  slice_split.u32SplitMode = (RK_U32)config->video.slice_split_mode;
  slice_split.u32SplitSize = (RK_U32)config->video.slice_split_size;

  ret = RK_MPI_VENC_SetSliceSplit(config->video.venc_channel, &slice_split);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_SetSliceSplit failed %#x", ret);
    return;
  }

  IPC_LITE_LOGI("pipeline", "slice split enabled mode=%u size=%u",
                slice_split.u32SplitMode, slice_split.u32SplitSize);
}

static int venc_init(const IPC_LITE_CONFIG *config) {
  VENC_CHN_ATTR_S attr;
  VENC_CHN_REF_BUF_SHARE_S ref_buf_attr;
  VENC_RECV_PIC_PARAM_S recv_param;
  RK_U32 venc_buffer_size = 0;
  int ret = 0;

  memset(&attr, 0, sizeof(attr));
  memset(&ref_buf_attr, 0, sizeof(ref_buf_attr));
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

  attr.stVencAttr.u32MaxPicWidth = (RK_U32)config->video.max_width;
  attr.stVencAttr.u32MaxPicHeight = (RK_U32)config->video.max_height;
  attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  attr.stVencAttr.enMirror = MIRROR_NONE;
  venc_buffer_size = effective_venc_buffer_size(config);
  attr.stVencAttr.u32BufSize = venc_buffer_size;
  attr.stVencAttr.bByFrame = RK_TRUE;
  attr.stVencAttr.u32PicWidth = (RK_U32)config->video.width;
  attr.stVencAttr.u32PicHeight = (RK_U32)config->video.height;
  attr.stVencAttr.u32VirWidth = IPC_LITE_ALIGN((RK_U32)config->video.width, 16);
  attr.stVencAttr.u32VirHeight =
      IPC_LITE_ALIGN((RK_U32)config->video.height, 16);
  attr.stVencAttr.u32StreamBufCnt = (RK_U32)config->video.venc_buffer_count;

  ret = RK_MPI_VENC_CreateChn(config->video.venc_channel, &attr);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGE("pipeline", "RK_MPI_VENC_CreateChn failed %#x", ret);
    return -1;
  }

  sync_frame_rate(config);

  if (config->video.codec == IPC_LITE_CODEC_H264) {
    apply_h264_defaults(config->video.venc_channel);
  } else {
    apply_h265_defaults(config->video.venc_channel);
  }

  apply_scene_mode(config);
  apply_channel_params(config);
  apply_motion_tuning(config);
  apply_slice_split(config);

  ref_buf_attr.bEnable = config->video.enable_refer_buffer_share ? RK_TRUE
                                                                  : RK_FALSE;
  ret = RK_MPI_VENC_SetChnRefBufShareAttr(config->video.venc_channel,
                                          &ref_buf_attr);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline",
                  "RK_MPI_VENC_SetChnRefBufShareAttr failed %#x", ret);
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
      packet.data = base;
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

int ipc_lite_pipeline_open_disabled_sinks(IPC_LITE_MPI_PIPELINE *pipeline,
                                          const IPC_LITE_CONFIG *config) {
  size_t i = 0;

  if (!pipeline || !config) {
    return -1;
  }

  pipeline->config = config;
  for (i = 0; i < pipeline->sink_count; ++i) {
    if (pipeline->sinks[i].enabled || !pipeline->sinks[i].open) {
      continue;
    }

    if (pipeline->sinks[i].open(&pipeline->sinks[i], config,
                                config->video.codec) != 0) {
      IPC_LITE_LOGE("pipeline", "failed to open sink %s",
                    pipeline->sinks[i].name);
      return -1;
    }
  }

  return 0;
}

int ipc_lite_pipeline_request_idr(IPC_LITE_MPI_PIPELINE *pipeline,
                                  bool instant) {
  int ret = 0;

  if (!pipeline || !pipeline->config || !pipeline->venc_enabled) {
    return -1;
  }

  ret = RK_MPI_VENC_RequestIDR(pipeline->config->video.venc_channel,
                               instant ? RK_TRUE : RK_FALSE);
  if (ret != RK_SUCCESS) {
    IPC_LITE_LOGW("pipeline", "RK_MPI_VENC_RequestIDR failed %#x", ret);
    return -1;
  }

  IPC_LITE_LOGI("pipeline", "requested IDR (instant=%d)", instant ? 1 : 0);
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
