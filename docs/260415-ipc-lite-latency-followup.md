# ipc_lite 720p60 延迟排查补充记录

日期：2026-04-15

## 1. 目的

在完成 `mis5001` 原生 `1280x720@60` bring-up 之后，继续排查：

- 为什么链路已经稳定跑到 `60fps`
- 接收端也已经做了低延迟处理
- 但整机观感延迟仍然在 `100ms ~ 150ms`

本轮的重点不是继续“盲调 ini”，而是把已经尝试过的方向系统记下来，避免后续重复试错。

## 2. 当前前提

本轮排查基于以下事实展开：

- `mis5001` 已支持原生 `1280x720` mode
- `run.sh` 启动前会按配置用 `media-ctl` 强制切到对应 sensor mode
- `ipc_lite.720p60.fast.udp.ini` 可以稳定输出 `1280x720@60`
- `fps=70+` 的问题已经通过修正 `VTS` 解决，不再是当前主问题

因此，本轮要找的是“60fps 稳定之后，额外延迟还藏在哪一段”。

## 3. 本轮做过的尝试

### 3.1 RTSP 旁路：增加 fast UDP 推流路径

目的：

- 排除板端 RTSP server 带来的额外缓存和排队

做法：

- 为 `ipc_lite` 增加 `udp_rtp_sink`
- 新增 `ipc_lite.720p60.fast.udp.ini`
- 继续使用同一个 `ipc_lite` 二进制，只是切到 UDP RTP 输出配置

结论：

- 可以降低一部分链路复杂度
- 但不能解释剩余的 `100ms ~ 150ms` 延迟
- 说明主问题不在 RTSP server 这一层

当前状态：

- 这部分改动仍然保留，但只保留一个二进制名：
  - `ipc_lite`
  - `ipc_lite.720p60.fast.udp.ini`

### 3.2 low delay 近似项：`s32MaxReEncodeTimes = 0`

目的：

- 给 `ipc_lite` 补一个尽量接近 `base:low_delay=1` 的公开 API 配置

做法：

- 通过 `RK_MPI_VENC_GetRcParam/SetRcParam`
- 对 `H.264/H.265` 都尝试设置：
  - `s32MaxReEncodeTimes = 0`

结论：

- 没观察到足以解释主延迟的改善
- 也没有建立出“它是关键根因”的证据

当前状态：

- 已从仓库代码中撤回，不作为当前默认路径保留

### 3.3 压缩 buffer：把 `input_buffer_count / venc_buffer_count` 压到 1

目的：

- 粗暴减少显式 buffer 数量，观察延迟是否下降

做法：

- 临时把：
  - `rk_aiq_uapi2_sysctl_preInit_devBufCnt(..., 1)`
  - `input_buffer_count = 1`
  - `venc_buffer_count = 1`

结论：

- 板端并未表现出更低延迟
- 实际主观观感反而更差
- 同时内部 `mb pool` 的表现也没有体现出预期收益

当前状态：

- 已回退到：
  - `input_buffer_count = 2`
  - `venc_buffer_count = 2`
- 不再把“buffer 全压到 1”作为当前方向

### 3.4 直送实验：`VI_GetChnFrame -> VENC_SendFrame`

目的：

- 排除 `RK_MPI_SYS_Bind(VI -> VENC)` 这条默认路径内部可能存在的隐藏排队

做法：

- 临时给 `ipc_lite` 增加一条 direct path：
  - `RK_MPI_VI_GetChnFrame`
  - `RK_MPI_VENC_SendFrame`
  - `RK_MPI_VI_ReleaseChnFrame`
- 配套新增过一份 `ipc_lite.720p60.direct.udp.ini`

结论：

- 在 direct path 下仍然可以稳定 `60fps`
- 但没有找到足以解释全部延迟的证据
- 这条路径更适合做诊断，不适合作为当前默认实现长期保留

当前状态：

- direct path 配置和代码都已从仓库撤回

### 3.5 “丢旧帧”实验：只保留最新帧

目的：

- 验证 `VI` 用户态可见队列里是否已经堆了多帧旧帧

做法：

- 在 direct path 上再加一层：
  - 首次阻塞取一帧
  - 然后用 `timeout=0` 循环继续取
  - 若还能取到，释放旧帧，仅保留最新帧送编码
- 统计：
  - `drained`

结论：

- `drained` 持续为 `0`
- 没有看到“用户态可见 VI 队列里堆了多帧旧帧”的证据

当前状态：

- 这套代码已撤回，仅保留本条记录

### 3.6 板端时序埋点：`frame_age_ms / submit_to_stream_ms / get_wait_ms / pts_delta_ms`

目的：

- 把“帧老不老”从感觉问题变成可量化问题

临时加过的指标：

- `frame_age_ms`
  - `now_monotonic_us - frame.stVFrame.u64PTS`
- `submit_to_stream_ms`
  - `VENC_SendFrame` 成功到 `GetStream` 拿到同 `PTS` 码流之间的时间
- `get_wait_ms`
  - `RK_MPI_VI_GetChnFrame()` 的阻塞等待时间
- `pts_delta_ms`
  - 相邻帧 `u64PTS` 间隔

结论：

- direct path 下多轮实测大致为：
  - `frame_age_ms = 13 ~ 23`
  - `submit_to_stream_ms = 4 ~ 6`
  - `pts_delta_ms = 16`
  - `get_wait_ms = 6 ~ 16`
  - `drained = 0`

这说明：

- `VI_GetChnFrame()` 不是在连续捞一串已经堆积的旧帧
- 当前用户态可见的 `VI` 输出队列里，没有明显 backlog
- `VI -> VENC -> GetStream` 这段板端可见时延并不大

更合理的解释变成：

- `u64PTS` 很可能是在 ISP/VI 较后的位置才打上
- 真正的延迟更可能藏在：
  - sensor
  - ISP 前段
  - 或 `rkisp_selfpath` 自身内部 pipeline depth

当前状态：

- 这些埋点都已从仓库撤回，只保留结论

## 4. 当前结论

到本轮结束时，可以先确认以下几点：

1. 当前主延迟大概率不在：
   - RTSP server
   - `VI -> VENC` 绑定方式
   - 用户态可见的 VI 输出队列 backlog

2. 当前更可疑的是：
   - ISP 前半段
   - `rkisp_selfpath`
   - AIQ/3A 带来的 pipeline depth
   - 或更早于 `u64PTS` 打点位置的内部缓存

3. 因此后续排查不应再继续反复试：
   - `drop_stale`
   - `direct_venc_feed`
   - `buffer_count = 1`
   - `max_reenc = 0`

除非有新证据，否则这些方向已经不值得继续投入。

## 5. 已撤回的实验性改动

本次记录完成后，以下只用于定位的改动已经从仓库撤回：

- `direct_venc_feed`
- `drop_stale_frames`
- direct path 相关线程和逻辑
- 延迟/PTS 诊断埋点
- `ipc_lite.720p60.direct.udp.ini`
- `ipc_lite.720p60.fast.udp.noaiq.ini`
- `max_reenc = 0` 的 low-delay 近似设置

当前仓库仍保留的有用改动只有：

- `ipc_lite.720p60.fast.udp.ini`
- `udp_rtp_sink`
- 单一入口的 `run.sh`
- `mis5001` 原生 `1280x720@60` 模式和对应 bring-up 路径

## 6. 下一步建议

如果继续追主延迟，优先级建议改成：

1. 直接对比 `rkisp_selfpath` 底层 `v4l2 buffer timestamp` 与 `frame.stVFrame.u64PTS`
2. 对比 `selfpath / mainpath / bypasspath` 在同一 sensor mode 下的实际延迟差异
3. 暂时关闭或弱化 AIQ 中最可能增加 pipeline depth 的路径，再做 A/B

换句话说，下一步应该往 ISP/VI 前段继续打，而不是再折腾编码器后段。
