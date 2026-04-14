#include "isp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static XCamReturn isp_sof_callback(rk_aiq_metas_t *meta) {
  (void)meta;
  return XCAM_RETURN_NO_ERROR;
}

static XCamReturn isp_err_callback(rk_aiq_err_msg_t *msg) {
  IPC_LITE_LOGW("isp", "rkaiq error callback code=%d", msg->err_code);
  return XCAM_RETURN_NO_ERROR;
}

static void sync_sensor_frame_rate(IPC_LITE_ISP_CONTEXT *context,
                                   const IPC_LITE_CONFIG *config) {
  Uapi_ExpSwAttrV2_t exp_sw_attr;
  int ret = 0;

  if (!config->video.sync_sensor_fps || config->video.fps <= 0 ||
      !context->aiq_ctx) {
    return;
  }

  memset(&exp_sw_attr, 0, sizeof(exp_sw_attr));
  ret = rk_aiq_user_api2_ae_getExpSwAttr(context->aiq_ctx, &exp_sw_attr);
  if (ret != 0) {
    IPC_LITE_LOGW("isp", "rk_aiq_user_api2_ae_getExpSwAttr failed %d", ret);
    return;
  }

  exp_sw_attr.stAuto.stFrmRate.isFpsFix = true;
  exp_sw_attr.stAuto.stFrmRate.FpsValue = (float)config->video.fps;
  ret = rk_aiq_user_api2_ae_setExpSwAttr(context->aiq_ctx, exp_sw_attr);
  if (ret != 0) {
    IPC_LITE_LOGW("isp",
                  "rk_aiq_user_api2_ae_setExpSwAttr failed %d for fps=%d",
                  ret, config->video.fps);
    return;
  }

  IPC_LITE_LOGI("isp", "sensor fps fixed to %d", config->video.fps);
}

int ipc_lite_isp_start(IPC_LITE_ISP_CONTEXT *context,
                       const IPC_LITE_CONFIG *config) {
  rk_aiq_static_info_t static_info;
  char hdr_mode[16];
  const char *sensor_name = NULL;

  memset(context, 0, sizeof(*context));

  if (!config->isp.enable_aiq) {
    IPC_LITE_LOGI("isp", "aiq disabled by config");
    return 0;
  }

  context->cam_id = config->isp.cam_id;

  snprintf(hdr_mode, sizeof(hdr_mode), "%d", config->isp.hdr_mode);
  setenv("HDR_MODE", hdr_mode, 1);

  memset(&static_info, 0, sizeof(static_info));
  rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(context->cam_id, &static_info);
  sensor_name = static_info.sensor_info.sensor_name;

  if (sensor_name && sensor_name[0] != '\0') {
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(sensor_name, "rkraw_rx", 2);
    rk_aiq_uapi2_sysctl_preInit_scene(sensor_name, "normal", "day");
  }

  IPC_LITE_LOGI("isp", "sensor=%s iq_dir=%s",
                sensor_name ? sensor_name : "-", config->isp.iq_dir);

  context->aiq_ctx = rk_aiq_uapi2_sysctl_init(
      sensor_name, config->isp.iq_dir, isp_err_callback,
      isp_sof_callback);
  if (!context->aiq_ctx) {
    IPC_LITE_LOGE("isp", "rk_aiq_uapi2_sysctl_init failed");
    return -1;
  }

  if (rk_aiq_uapi2_sysctl_prepare(context->aiq_ctx, 0, 0,
                                  RK_AIQ_WORKING_MODE_NORMAL) != 0) {
    IPC_LITE_LOGE("isp", "rk_aiq_uapi2_sysctl_prepare failed");
    rk_aiq_uapi2_sysctl_deinit(context->aiq_ctx);
    context->aiq_ctx = NULL;
    return -1;
  }

  if (rk_aiq_uapi2_sysctl_start(context->aiq_ctx) != 0) {
    IPC_LITE_LOGE("isp", "rk_aiq_uapi2_sysctl_start failed");
    rk_aiq_uapi2_sysctl_deinit(context->aiq_ctx);
    context->aiq_ctx = NULL;
    return -1;
  }

  context->enabled = true;
  sync_sensor_frame_rate(context, config);
  IPC_LITE_LOGI("isp", "aiq started");
  return 0;
}

void ipc_lite_isp_stop(IPC_LITE_ISP_CONTEXT *context) {
  if (!context->enabled || !context->aiq_ctx) {
    return;
  }

  IPC_LITE_LOGI("isp", "stopping aiq");
  rk_aiq_uapi2_sysctl_stop(context->aiq_ctx, false);
  rk_aiq_uapi2_sysctl_deinit(context->aiq_ctx);
  memset(context, 0, sizeof(*context));
}
