#ifndef IPC_LITE_ISP_H
#define IPC_LITE_ISP_H

#include <stdbool.h>

#include "config.h"

#ifdef RV1126_RV1109
#include <rk_aiq_user_api_sysctl.h>
#else
#include <rk_aiq_user_api2_sysctl.h>
#endif

typedef struct {
  bool enabled;
  int cam_id;
  rk_aiq_sys_ctx_t *aiq_ctx;
} IPC_LITE_ISP_CONTEXT;

int ipc_lite_isp_start(IPC_LITE_ISP_CONTEXT *context,
                       const IPC_LITE_CONFIG *config);
void ipc_lite_isp_stop(IPC_LITE_ISP_CONTEXT *context);

#endif

