# ipc_lite 推流脚本延迟 / FPS / 720p 稳定性迭代记录

日期：2026-04-14

## 1. 目标

基于：

- `docs/260413-ipc-lite-h264-investigation.md`

继续迭代 `project/app/ipc_lite/` 的推流脚本与编码路径，目标按优先级拆成三层：

1. 先降低编码与首帧可见延迟
2. 再提升稳定输出帧率
3. 最终冲击 `720p + 高帧率 + 稳定`

本轮不把“端口能连上”视为成功，必须同时关注：

- 实际可见画面
- 持续输出 `fps`
- 实际码率
- 启动成功率
- 自动恢复后的对外行为

## 2. 已知前提

上一轮已经确认：

- `704x576 + selfpath + H.264` 已被应用层 `preflight` 收敛到“对外不再暴露坏流”
- `H.264` 在 `2048 kbps + gop=25` 下优于旧基线
- `mis5001` 仍伴随：
  - `fs/fe mis`
  - `csi size err`
  - `PIC_SIZE_ERROR`
- `720p` 在此前调查中还不能视为稳定路径

因此本轮的核心不是“从零把流跑通”，而是：

- 把 `ipc_lite` 的编码器参数面板做得更接近 `rkipc`
- 系统验证哪些参数真的能换来更低延迟 / 更高 fps
- 判断当前 `720p` 的真正瓶颈是在 `VENC` 还是更下游的 `sensor/CSI/ISP`

## 3. 初始工作计划

### 3.1 代码侧

- 对照 `rkipc rv1106_ipc` 的 `VI/VENC` 初始化
- 梳理当前 `ipc_lite` 缺失的编码调优项
- 给 `ipc_lite.ini` 增加必要的低延迟 / 高 fps 调优参数入口
- 视结果调整 `run.sh` 与默认配置

### 3.2 实机侧

- 先回归当前 `704x576` 健康基线
- 再测试更激进的 `gop/fps/bitrate/profile/buffer` 组合
- 之后尝试回到 `1280x720`
- 记录每组配置下：
  - 启动行为
  - 板端 `stats`
  - RTSP 可拉流性
  - 客户端观感与平均码率
  - 内核异常是否同步放大

### 3.3 资料侧

- 继续对照仓库内 `rkipc/RKADK` 实现
- 必要时补充外部资料，优先找 Rockchip 官方文档、官方示例和主源码

## 4. 当前基线观察

截至开工时，已确认：

- 板端网络可达：`172.20.10.12`
- 板端工作目录：`/root/ipc_lite`
- 板端保留了上一轮多组配置，便于直接回归
- `ipc_lite` 当前代码仍只有较少的显式 VENC 调优参数
- 相比 `rkipc`，`ipc_lite` 仍未暴露：
  - `rc_mode`
  - `gop_mode`
  - `rc_quality`
  - `frame_min/max_qp`
  - `qbias`
  - `filter`
  - `anti_ring`
  - `anti_line`
  - `lambda`
  - `motion_deblur`
  - `motion_static_switch`
  - `debreath`

这意味着本轮很可能需要先补“可调性”，再谈系统优化。

## 5. 过程记录

### 5.1 仓库内对照结论

已完成的第一轮代码对照：

- `ipc_lite/src/mpi_pipeline.c` 目前固定使用：
  - `H264CBR/H265CBR`
  - 固定 `profile`
  - 固定一组 `RcParam / RcParam2 / qbias / filter / anti_ring / anti_line / lambda`
- `rkipc/src/rv1106_ipc/video/video.c` 已具备成体系的参数入口，可从 ini 控制：
  - `CBR/VBR`
  - `normalP/smartP/TSVC4`
  - `frame_min/max_qp`
  - `rc_quality`
  - `motion_deblur`
  - `debreath`
  - `RcParam2`
  - `Qbias`
  - `filter`
  - `anti_ring`
  - `anti_line`
  - `lambda`
  - `refer_buffer_share`

第一判断：

- `ipc_lite` 目前更像“最小可运行版本”
- 还不是“可调优版本”
- 如果目标是压延迟、提 fps、冲击 720p，必须先把关键控制面做出来

### 5.2 板端连通性

已确认：

- `ping 172.20.10.12` 正常
- `ssh root@172.20.10.12` 正常
- 板端已有多份上一轮 `ipc_lite` 测试配置，可直接用于回归

## 6. 下一步

接下来优先执行：

1. 盘点板端现有可复用脚本与工具
2. 回归一轮当前 `704x576 + H.264` 基线
3. 补 `ipc_lite` 的可调编码参数与实验脚本
4. 再开始 `720p` 与高 fps 冲刺

## 7. 新增发现：内存与文件输出是必须显式控制的约束

板端当前可用内存非常小：

- `MemTotal: 33644 kB`
- 无 swap
- `/tmp` 是 `tmpfs`

这意味着：

- `output.file.path = /tmp/...` 实际是在吃 RAM
- 长时间把裸码流写到 `/tmp`，会直接把系统推向 `OOM`

本轮还从板端历史 `dmesg` 里确认到过一次真实 `OOM`：

- `ipc_lite` 曾被 `oom-killer` 杀掉

因此本轮后续所有“面向 RTSP 的性能实验”都统一改成：

- `file = off`
- 只在需要抓本地样本时短时打开 file sink

这不是体验优化，而是当前板级资源约束下的硬性前提。

## 8. 720p 重新验证：问题不再能简单归因于“720p 本身不行”

### 8.1 关键修正

重新验证 `720p` 前，本轮先修正了一个此前容易被忽略的条件：

- `1280x720` 不再沿用 `704x576` 的小 `venc_buffer_size`
- 统一改为：
  - `venc_buffer_size = 1382400`
  - 即 `1280 * 720 * 3 / 2`

### 8.2 `720p + selfpath + H.264 + file/rtsp 全关`

配置：

- `1280x720`
- `H.264`
- `vi_channel = 1`
- `fps = 25`
- `gop = 25`
- `bitrate_kbps = 3072`
- `file = off`
- `rtsp = off`

结果：

- 可以稳定持续出码
- 板端统计大致在：
  - `21.4 ~ 24.5 fps`
  - `266 ~ 637 kbps`
- 底层仍持续出现：
  - `fs/fe mis`
  - `csi size err`
  - `PIC_SIZE_ERROR`

结论：

- `720p selfpath` 不是“完全起不来”
- 它在当前板级异常背景下，已经具备继续优化的价值

### 8.3 `720p + mainpath + H.264 + file/rtsp 全关`

同样修正 buffer 后，`mainpath(vi=0)` 也不是完全启动失败：

- 前几个统计窗口能到：
  - `23 ~ 25 fps`
- 但更容易迅速退化到：
  - `72 kbps`
  - `55 kbps`
  - `last_len` 很小

结论：

- 在当前板上，`mainpath` 明显劣于 `selfpath`
- 后续若目标是 `720p + 稳定`，应优先押注：
  - `rkisp_selfpath`

## 9. 720p RTSP 基线重建

### 9.1 旧路径的重测结果

在不修改代码、只使用更合理的 `720p selfpath + nofile + RTSP` 配置时，已经能拿到一条可用的 `RTSP`：

- `ffmpeg` 可稳定抓到约 `8s` 码流
- `ffprobe` 结果：
  - `duration ≈ 8.20s`
  - `size = 447816`
  - `bit_rate ≈ 436876`
- 客户端识别到：
  - `1280x720`
  - `25 fps`

板端对应统计大致在：

- `21.4 ~ 26.9 fps`
- `286 ~ 402 kbps`

这条结果很重要，因为它说明：

- `720p RTSP` 已经不再是“理论目标”
- 在当前板上可以真实拉到有效 `H.264`

但它还不够好，主要问题是：

- 实际码率偏低
- 起播时延仍不够理想
- `fps` 还没有逼近 `30`

## 10. 高帧率冲击：单纯把 `fps=30` 并不成立

本轮专门做了 `720p selfpath + H.264 + fps=30 + gop=30 + 4096 kbps` 短跑。

结果：

- 板端配置层和 MPP 层都显示已按 `30 fps` 下发
- 但板端实际统计仍大致停在：
  - `21.9 ~ 23.0 fps`
- 实际输出码率反而比 `25 fps` 路径更差

结论：

1. 当前瓶颈不是“我们忘了把 VENC 的 fps 改成 30”
2. 单纯把 `video.fps` 改成 `30`，不能把实际输出提升到 `30 fps`
3. 若想继续冲高帧率，必须把：
   - sensor / AE frame rate
   - VI / VENC frame rate
   统一起来

## 11. 本轮代码改动

### 11.1 `isp.c`：补上 sensor fps 同步

新增：

- 在 `AIQ start` 完成后，调用：
  - `rk_aiq_user_api2_ae_getExpSwAttr()`
  - `rk_aiq_user_api2_ae_setExpSwAttr()`

作用：

- 把 sensor / AE 的 `stFrmRate.FpsValue` 显式同步到 `config.video.fps`

这一步之前是缺失的。

### 11.2 `config.*`：把低延迟 / 高 fps 相关参数显式化

新增配置项：

- `sync_sensor_fps`
- `scene_mode`
- `enable_motion_deblur`
- `motion_deblur_strength`
- `enable_motion_static_switch`
- `enable_slice_split`
- `slice_split_mode`
- `slice_split_size`
- `max_stream_count`
- `poll_wakeup_frame_count`
- `request_idr_on_start`
- `request_idr_on_rtsp_enable`

### 11.3 `mpi_pipeline.c`：接入新的 VENC 调优能力

新增能力：

- 对大于 `704x576` 的分辨率，若 `venc_buffer_size` 过小则自动抬高到保守值
- `RK_MPI_VENC_SetSceneMode()`
- `RK_MPI_VENC_SetChnParam()`：
  - `u32MaxStrmCnt`
  - `u32PollWakeUpFrmCnt`
- `RK_MPI_VENC_EnableMotionDeblur()`
- `RK_MPI_VENC_EnableMotionStaticSwitch()`
- `RK_MPI_VENC_SetSliceSplit()`
- `RK_MPI_VENC_RequestIDR()`

### 11.4 `app.c`：在关键时机主动请求 IDR

新增两次 IDR 请求：

1. pipeline 启动后
2. preflight 通过、RTSP 真正开放后

目的：

- 缩短客户端连接后等待下一个 IDR 的时间

### 11.5 `run.sh` 与安装产物

`run.sh` 现在支持：

- 无参数时使用 `IPC_LITE_CONFIG`
- 单个位置参数直接视为配置文件

同时新增并安装了新的预设配置：

- `project/app/ipc_lite/ipc_lite.720p.lowlat.rtsp.ini`

## 12. 改动后的实测

### 12.1 `720p + 30fps lowlat`：起播更快，但码率明显变差

代码改完后，先试了：

- `720p`
- `selfpath`
- `fps = 30`
- `scene_mode = cvr`
- `sync_sensor_fps = true`
- `max_stream_count = 1`
- `poll_wakeup_frame_count = 1`
- `IDR on start / rtsp enable`

结果：

- 板端明确打印：
  - `sensor fps fixed to 30`
- 客户端 `ffmpeg` 看到的 `start` 值约：
  - `0.333`
- 但实际抓流结果明显变差：
  - `bit_rate ≈ 149 kbps`

这说明：

- `sensor fps` 的确被同步了
- 但当前板子在 `720p30` 下无法同时维持高码率和稳定输出

因此：

- `720p30` 暂时不应作为默认预设

### 12.2 `720p + 25fps lowlat`：本轮最优解

随后把同样的 low-latency 组合退回到 `25 fps`：

- `720p`
- `selfpath`
- `fps = 25`
- `gop = 25`
- `bitrate_kbps = 3072`
- `sync_sensor_fps = true`
- `scene_mode = cvr`
- `file = off`
- `max_stream_count = 1`
- `poll_wakeup_frame_count = 1`
- `request_idr_on_start = true`
- `request_idr_on_rtsp_enable = true`

结果明显提升：

板端统计：

- `24.4 ~ 25.4 fps`
- `1507 ~ 2672 kbps`

主机侧 `ffprobe`：

- `duration ≈ 8.24s`
- `size = 2263520`
- `bit_rate ≈ 2197491`

客户端识别：

- `1280x720`
- `25 fps`

结论：

- 这是截至本轮最接近“720p + 稳定 + 低延迟”的组合
- 与改动前的 `720p RTSP` 基线相比：
  - 实际码率从约 `437 kbps` 提升到约 `2.20 Mbps`
  - `fps` 更稳定地贴近 `25`

### 12.3 `slice split` 追加实验

在上述 `25fps lowlat` 基线上，再试：

- `enable_slice_split = true`
- `slice_split_mode = 0`
- `slice_split_size = 4096`

结果：

- 客户端 `start` 值缩短到约：
  - `0.440`
- 但实际抓流平均码率下降到约：
  - `1.73 Mbps`

结论：

- `slice split` 对首帧等待有正向帮助
- 但会明显拉低当前板上的实际输出码率
- 在本轮结论里，它更适合保留为：
  - 可选实验项
  - 而不是默认项

## 13. 本轮推荐配置

截至当前，推荐把 `720p` 默认探索基线更新为：

- `1280x720`
- `H.264`
- `vi_channel = 1`
- `fps = 25`
- `gop = 25`
- `bitrate_kbps = 3072`
- `venc_buffer_size = 1382400`
- `sync_sensor_fps = true`
- `scene_mode = cvr`
- `max_stream_count = 1`
- `poll_wakeup_frame_count = 1`
- `request_idr_on_start = true`
- `request_idr_on_rtsp_enable = true`
- `file = off`
- `rtsp = on`

对应预设文件：

- `project/app/ipc_lite/ipc_lite.720p.lowlat.rtsp.ini`

板端运行方式：

```sh
cd /root/ipc_lite
./run.sh ./ipc_lite.720p.lowlat.rtsp.ini
```

或：

```sh
cd /root/ipc_lite
IPC_LITE_CONFIG=./ipc_lite.720p.lowlat.rtsp.ini ./run.sh
```

## 14. 当前结论

本轮最重要的结论有四条：

1. `720p` 不是当前板子的绝对禁区
2. `mainpath` 明显不如 `selfpath` 稳定
3. `720p30` 目前不成立，`720p25` 才是可用的收敛点
4. 把 sensor fps、IDR、低缓冲和 file sink 策略一起收敛后，`ipc_lite` 已经能给出一条真实可用的 `720p RTSP` 路径

更保守但更准确的表述是：

- 当前还没有达到“720p 高帧率”的最终目标
- 但已经把目标从“704x576 保守基线”推进到了：
  - `720p + 25fps + 稳定可拉流`

## 15. 后续建议

若继续推进，优先顺序建议改成：

1. 先把当前 `720p25 lowlat` 作为新的稳定工作基线
2. 长跑验证：
   - `10 min`
   - `30 min`
   - 看是否再出现 OOM / 掉码 / 坏流
3. 只在长跑稳定后，再继续冲：
   - `720p30`
4. 若要继续冲低延迟，可优先追加验证：
   - 更小 `gop`
   - `slice split`
   - `motion_deblur`
5. 若要继续冲高帧率，重点应转向：
   - `mis5001` 当前单 mode 的实际输出极限
   - `sensor/CSI/ISP` 的 `fs/fe mis + csi size err + PIC_SIZE_ERROR`

一句话总结本轮进展：

- `ipc_lite` 已经从“704x576 保守可用”推进到“720p25 可稳定 RTSP 拉流”，而不是还停留在 720p 无法落地的阶段

## 16. 驱动层新结论：高帧率瓶颈很可能不在 `ipc_lite`

继续往下核对 `mis5001` 驱动后，得到一个比 `rkipc` 更关键的发现：

- [`sysdrv/source/kernel/drivers/media/i2c/mis5001.c`](/work/luckfox-pico/sysdrv/source/kernel/drivers/media/i2c/mis5001.c) 当前只注册了一个 `supported_modes[]`
- 这个 mode 是：
  - `2592x1944`
  - `2 lane`
  - `RAW10`
- 没有任何原生 `1920x1080` 或 `1280x720` mode

这意味着当前我们在应用层跑的：

- `1280x720`
- `rkisp_selfpath`

更大概率是：

- sensor 仍输出全分辨率
- ISP / VI 再把它缩到 `720p`

因此：

- 单纯继续在 `ipc_lite` 里把 `video.fps` 改成 `30`
- 或继续照搬 `rkipc` 的编码参数

都不太可能从根本上把帧率抬上去。

### 16.1 一个需要特别警惕的细节

当前 `mis5001.c` 里还有一个可疑点：

- mode 注释写的是：
  - `Linear 25Fps`
- 但 `supported_modes[]` 里的 `max_fps` 却填成：
  - `.numerator = 10000`
  - `.denominator = 300000`
  - 即按驱动表达更接近 `30 fps`

这说明至少存在两种可能：

1. 注释过期，真实寄存器表已经按 `30 fps` 时序在跑
2. 寄存器表仍更接近 `25 fps`，但驱动却把 mode 上限按 `30 fps` 暴露给了上层

如果是第 2 种情况，就很容易出现我们当前看到的现象：

- 上层配置与日志都显示 `30 fps`
- 实际输出却长期卡在 `21 ~ 23 fps`
- 并伴随 `fs/fe mis`、`csi size err`、`PIC_SIZE_ERROR`

### 16.2 后续推进顺序需要调整

基于这个新发现，后续工作建议拆成两条线：

#### A. 用户态继续做，但目标变成“排除编码器瓶颈”

- 给 `ipc_lite` 增加更多编码器调优入口
- 用更轻的 `profile / rc / qp / anti_ring / anti_line` 组合测试
- 如果这样做后 `720p25` 还能继续明显提升，说明编码器端还有余量

#### B. 真正要冲高帧率，优先转向 `mis5001` 驱动

重点不再是：

- 再换一组 `VENC` 参数

而是：

1. 明确 `mis5001` 当前寄存器表到底对应 `25 fps` 还是 `30 fps`
2. 校正 `supported_modes[]` 的 `max_fps` 声明，避免上层被错误能力误导
3. 如果 sensor 支持更低分辨率高帧率模式：
   - 给驱动新增原生 `1920x1080` / `1280x720` mode
4. 让上层真正选择 sensor 原生 mode，而不是继续走“全分辨率进 ISP 再缩小”的路径

更直接地说：

- `ipc_lite` 还能继续优化
- 但“高帧率”这件事现在已经不能只当作应用层问题处理
- 如果目标是 `720p30` 甚至更高，`mis5001` 驱动和 sensor mode 基本已经进入关键路径

## 17. 高帧率目标继续上探：应转向“原生 sensor mode”，而不是只改应用层 fps

在明确提出“高帧率不能止步于 `30fps`，可以接受更低分辨率甚至 `120fps`”之后，本轮又补查了公开资料，结论比前面更清楚：

- `RV1106` 芯片规格本身不是关键限制
- 当前 Luckfox `MIS5001` 模组使用的是 `2-lane MIPI CSI-2`
- `MIS5001` 公开规格里也明确存在偏视频场景的更高吞吐能力

换句话说：

- `1080p60`
- `720p120`

在“链路带宽”层面并非完全没有可能

但当前仓库里的现实约束是：

1. `mis5001.c` 仍只有一个 `2592x1944` mode
2. 当前 Luckfox 官方论坛工程师给出的建议也是：
   - 现有 SDK 默认让 `MIS5001` 以最大分辨率 / 最大帧率往 `RV1106` 输出
   - 若只是想要低于该上限的分辨率 / 帧率，优先在软件层做缩放与限帧
   - 若要强行改 sensor 侧输出，则需要重新标定并匹配新的 `IQ` 文件

这意味着我们如果真的要推进：

- `1080p60`
- `720p120`

就不能只在：

- `ipc_lite.ini`
- `video.fps`
- `VENC rc/gop`

上继续打转，而要把工作拆成完整的三件事：

1. 拿到 `MIS5001` 对应低分辨率高帧率 mode 的寄存器表
2. 在 `mis5001` 驱动里把这些 mode 作为“原生 sensor mode”接进去
3. 为这些 mode 准备匹配的 `IQ` / ISP 调参

### 17.1 本轮驱动代码先做了哪一步

在还没有拿到新的 `1080p60/720p120` 寄存器表之前，本轮先把 `mis5001` 驱动整理成“可继续扩 mode”的状态：

- 文件：
  - [`sysdrv/source/kernel/drivers/media/i2c/mis5001.c`](/work/luckfox-pico/sysdrv/source/kernel/drivers/media/i2c/mis5001.c)

已新增 / 修正：

- 给 `mode` 补了：
  - `name`
  - `xvclk_freq`
  - `link_freq_idx`
- 给驱动实例补了：
  - `link_freq`
  - `pixel_rate`
  - 更一致的 `cur_vts` 状态维护
- 抽出 `mis5001_apply_mode_state()`：
  - 切 mode 时统一刷新：
    - `cur_mode`
    - `cur_vts`
    - `cur_fps`
    - `link_freq`
    - `pixel_rate`
    - `hblank/vblank`
- 在 `set_fmt()` 增加明确日志：
  - 若请求的是 `1280x720` 这类非原生尺寸，会直接打印当前仍落回哪个 native sensor mode
- 在 `stream on` 时打印：
  - 当前 sensor mode 名称
  - 分辨率
  - 目标 fps
  - `hts/vts`
  - `link_freq`
- 修正了旧驱动里明显误导的 `xvclk` 日志：
  - 过去打印成 `24MHz`
  - 实际这里一直按 `27MHz` 工作

这一步还没有把帧率直接推到 `60/120`

但它解决了一个更基础的问题：

- 以后继续加 `1080p60/720p120` mode 时
- 驱动内部的 mode 状态、控制项和日志不会继续混乱

### 17.2 这一轮新增的判断

补上本轮驱动替换后的一个实机验证结果：

- 已将新的 `mis5001.ko` 编译并替换到板端
- 在板端重新跑一轮 `ipc_lite.720p.lowlat.rtsp.ini` 后，内核明确打印：
  - `stream on with sensor mode 2592x1944@30 (2592x1944 @ 30.00 fps, hts=3000 vts=1980 link_freq=445500000)`
- 同一轮 `rockit/cmpi` 日志仍显示：
  - `sensor raw width = 2592`
  - `sensor raw height = 1944`

这条证据非常关键，因为它基本坐实了：

- 当前 `720p selfpath`
- 仍然是 `MIS5001 2592x1944` 原生输出进来
- 再由下游做裁剪 / 缩放

截至当前，更准确的判断应更新为：

1. `720p30` 没跑起来，不足以证明 `MIS5001` 或 `RV1106` 做不到更高 fps
2. 但当前仓库和板端软件栈，确实还没有为“原生低分辨率高帧率 mode”准备好
3. 真正要冲 `60fps / 120fps`，下一个硬前提不是继续换 `VENC` 参数，而是：
   - 补 mode 表
   - 补 IQ
   - 再做实机验证

### 17.3 建议的下一步

后续优先顺序建议更新为：

1. 继续搜集 `MIS5001` 手册或可复用的原厂寄存器表
2. 优先争取拿到：
   - `1920x1080@60`
   - `1280x720@120`
   这两档的原生 mode
3. 拿到 mode 后，先在 `mis5001.c` 增加多 mode 支持
4. 再补：
   - `rkipc`
   - `ipc_lite`
   对 sensor 原生 mode 的选择链路
5. 最后才进入：
   - `RTSP`
   - 编码器
   - 延迟 / 丢帧
   的系统联调

一句话总结这一步的意义：

- 本轮已经把问题正式从“应用层调参”推进到“原生 sensor mode + IQ”的层级
- 如果后续要拿到真正有价值的高帧率，路线应该是：
  - `MIS5001 native mode`
  - 而不是继续依赖 `2592x1944 -> ISP 缩放`
