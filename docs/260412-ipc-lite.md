# ipc_lite 方案与实施文档

## 1. 目标

在当前 Luckfox RV1106 Buildroot 仓库中新增一个独立应用：

- 路径：`project/app/ipc_lite/`
- 目标：实现精简、可读、可配置的用户态摄像头采集与视频编码程序
- 当前阶段优先级：
  - 跑通 `mis5001 -> ISP/VI -> VENC`
  - 分辨率先固定在 `1280x720`
  - 编码支持 `H.264` / `H.265`
  - 支持前台运行、信号退出、日志输出
  - 预留 `RTSP` / `RTMP` / `Wi-Fi status` 接口
- 交付要求：
  - 不走整镜像重编译/烧写
  - 只交叉编译二进制与配置文件
  - 通过串口终端把文件写入板子并完成运行验证

## 2. 现状确认

基于本次仓库和板端探索，当前环境成立：

- 主控：`RV1106`
- 工具链：`arm-rockchip830-linux-uclibcgnueabihf`
- 板端系统：`Buildroot 2023.02.6`
- 传感器：`mis5001`
- IQ 文件已存在：
  - `/oem/usr/share/iqfiles/mis5001_CMK-OT2115-PC1_30IRC-F16.json`
- 板端已具备 Rockchip 用户态媒体库：
  - `librockit`
  - `librockchip_mpp`
  - `librkaiq`
  - `librtsp`
- 板端可见摄像头相关节点：
  - `/dev/media0`
  - `/dev/media1`
  - `/dev/video11` 等

板端串口实测还能确认：

- `mis5001` 驱动已识别
- `rkcif` / `rkisp` / `mipi-csi2` 链路已建立
- 板端当前没有 `rkipc` 在运行
- 板端 `wlan0` 已拿到 IP，但当前项目第一阶段不依赖 Wi-Fi 推流联调

同时也观察到现有 sample 在 720p 编码时仍有：

- `mipi-csi2-hw ERR1:0x10`
- `rkcif-mipi-lvds: ERROR: csi size err`
- `rkisp-vir0: CIF_ISP_PIC_SIZE_ERROR`

这说明第一版 `ipc_lite` 的责任不是“彻底解决 sensor 时序问题”，而是：

- 用更可维护的代码把 AIQ、VI、VENC、码流输出和错误统计组织起来
- 让摄像头/编码链路的配置更清晰
- 为后续针对硬件特化优化留下稳定骨架

## 3. 设计边界

### 3.1 第一版要做

- 单进程
- 单 camera
- 单编码通道
- 单码流主通道
- 支持 `H.264/H.265`
- 支持 `ini` 配置
- 支持 AIQ 初始化
- 支持文件输出
- 支持 RTSP 输出
- 支持日志和周期统计
- 支持信号退出和资源清理
- 支持查询并打印 Wi-Fi 状态

### 3.2 第一版不做

- 音频
- 双码流/三码流
- OSD
- NPU
- 录像切片
- 复杂网络控制协议
- 真正可用的 RTMP 推流
- 自动联网管理

RTMP 在第一版只做接口层和配置项，不承诺当前板端可用。

## 4. 项目目录

计划新增如下结构：

```text
project/app/ipc_lite/
├── Makefile
├── CMakeLists.txt
├── ipc_lite.ini
├── run.sh
├── deploy_serial.py
└── src/
    ├── main.c
    ├── app.c
    ├── app.h
    ├── config.c
    ├── config.h
    ├── log.c
    ├── log.h
    ├── isp.c
    ├── isp.h
    ├── mpi_pipeline.c
    ├── mpi_pipeline.h
    ├── stream_sink.h
    ├── file_sink.c
    ├── file_sink.h
    ├── rtsp_sink.c
    ├── rtsp_sink.h
    ├── rtmp_sink.c
    ├── rtmp_sink.h
    ├── wifi_status.c
    └── wifi_status.h
```

说明：

- `Makefile` 负责接入当前 SDK 的 app 构建体系
- `CMakeLists.txt` 负责独立构建
- `deploy_serial.py` 负责把产物经串口写入板子
- `run.sh` 是板端启动脚本
- `src/` 内部按职责拆分，避免单文件堆叠成 `rkipc` 风格的大文件

## 5. 架构设计

### 5.1 模块关系

```text
ini config
   ↓
app bootstrap
   ├── logger
   ├── wifi status
   ├── isp(aiq)
   ├── rk mpi sys
   ├── vi
   ├── venc
   └── stream dispatcher
           ├── file sink
           ├── rtsp sink
           └── rtmp sink(stub)
```

### 5.2 运行流

1. 读取 `ipc_lite.ini`
2. 初始化日志
3. 安装 `SIGINT` / `SIGTERM` 处理
4. 初始化 AIQ
5. 初始化 `RK_MPI_SYS`
6. 初始化 `VI dev + VI chn`
7. 初始化 `VENC`
8. `VI -> VENC` 绑定
9. 启动码流线程：
   - `RK_MPI_VENC_GetStream`
   - 分发到文件输出 / RTSP / RTMP stub
   - 统计帧率、码率、错误数
10. 主线程周期打印状态：
   - uptime
   - encoded frames
   - bitrate
   - recent errors
   - wifi link/ip
11. 收到信号后按逆序释放资源

## 6. 配置文件设计

使用自定义的轻量 `ini` 解析，只支持本项目需要的键，不引入外部重型依赖。

### 6.1 配置分组

- `[app]`
- `[isp]`
- `[video]`
- `[output.file]`
- `[output.rtsp]`
- `[output.rtmp]`
- `[wifi]`
- `[debug]`

### 6.2 首版关键配置项

#### `[app]`

- `name`
- `log_level`
- `stats_interval_sec`

#### `[isp]`

- `enable_aiq`
- `iq_dir`
- `cam_id`
- `hdr_mode`

#### `[video]`

- `width`
- `height`
- `fps`
- `gop`
- `bitrate_kbps`
- `codec`
- `vi_channel`
- `pixel_format`

#### `[output.file]`

- `enable`
- `path`

#### `[output.rtsp]`

- `enable`
- `port`
- `path`

#### `[output.rtmp]`

- `enable`
- `url`

#### `[wifi]`

- `enable_status`
- `ifname`

#### `[debug]`

- `dump_first_frames`
- `venc_timeout_ms`

### 6.3 默认值

首版默认值按当前板子取保守配置：

- `width=704`
- `height=576`
- `max_width=704`
- `max_height=576`
- `fps=25`
- `gop=50`
- `bitrate_kbps=512`
- `codec=h265`
- `vi_channel=1`
- `venc_channel=1`
- `input_buffer_count=2`
- `venc_buffer_count=4`
- `venc_buffer_size=202752`
- `enable_refer_buffer_share=true`
- `enable_aiq=true`
- `iq_dir=/oem/usr/share/iqfiles`
- `rtsp.enable=false`
- `file.enable=true`
- `file.path=/tmp/ipc_lite.h265`
- `wifi.ifname=wlan0`

## 7. 编码与媒体实现策略

### 7.1 直接基于 RK MPI

不复用 `rkipc` 的大框架，直接调用：

- `RK_MPI_SYS_Init`
- `RK_MPI_VI_*`
- `RK_MPI_VENC_*`
- `RK_MPI_SYS_Bind`

理由：

- 依赖最少
- 结构清楚
- 便于定位硬件/配置问题
- 适合作为无人机图传最小原型

### 7.2 AIQ 处理

基于 sample 中的最小 AIQ 初始化逻辑：

- `rk_aiq_uapi2_sysctl_enumStaticMetas`
- `rk_aiq_uapi2_sysctl_init`
- `rk_aiq_uapi2_sysctl_prepare`
- `rk_aiq_uapi2_sysctl_start`
- `rk_aiq_uapi2_sysctl_stop`

并做两点约束：

- 只保留单 camera 路径
- 只暴露必要配置

### 7.3 RTSP

第一版直接接 `librtsp`：

- `create_rtsp_demo`
- `rtsp_new_session`
- `rtsp_set_video`
- `rtsp_tx_video`

这样可以在不引入额外框架的情况下，先把局域网拉流能力挂上。

### 7.4 RTMP

第一版不接入实际推流库，只做：

- 配置项
- 初始化函数
- 打桩日志
- 统一 sink 接口

以后如果要接：

- `librtmp`
- FFmpeg
- 自研 RTP/UDP

都可以从当前 `stream_sink` 接口扩展。

## 8. 日志与可观测性

### 8.1 日志目标

- 前台可读
- 级别明确
- 时间戳明确
- 出错时能定位到模块

### 8.2 日志级别

- `ERROR`
- `WARN`
- `INFO`
- `DEBUG`

### 8.3 周期状态输出

每 `stats_interval_sec` 输出一次：

- 运行秒数
- 总帧数
- 瞬时 fps
- 平均码率
- 最近一次 VENC PTS/长度
- RTSP 状态
- RTMP 状态
- Wi-Fi carrier / IP
- 累积错误数

## 9. 构建方案

### 9.1 Makefile

与现有 `project/app` 体系兼容：

- 读取 `../Makefile.param`
- 使用当前 SDK 已有的 include/lib
- 产物安装到：
  - `project/app/ipc_lite/out/`
  - `output/out/app_out/`

### 9.2 CMakeLists

提供独立构建能力：

- 可直接指定交叉编译器
- 明确 include/lib 搜索路径
- 显式链接：
  - `pthread`
  - `m`
  - `rockit`
  - `rockchip_mpp`
  - `rkaiq`
  - `rtsp`
  - `stdc++`

### 9.3 不重新编整镜像

本项目只需要：

1. 本地交叉编译
2. 取到二进制和配置文件
3. 优先用 `scp/ssh` 上传至板子 `/root/ipc_lite/`
4. 网络不可用时，再退回串口上传
5. 在板子上直接运行

## 10. 串口部署方案

### 10.1 原则

必须满足：

- 不重打镜像
- 不走升级工具
- 不依赖用户手工复制粘贴

同时，当前板子一旦接入可用 Wi-Fi，优先使用：

- `ssh`
- `scp`

只有在网络不可用时，才回退到串口上传。

### 10.2 方案

当前支持两种部署路径：

#### A. 推荐：网络部署

主机侧可直接：

1. `scp` 上传：
   - `ipc_lite`
   - `ipc_lite.ini`
   - `run.sh`
2. `ssh` 到板子执行：
   - `chmod +x`
   - `./run.sh`

优点：

- 速度远高于串口
- 更适合频繁迭代
- 可直接保留一个前台 `ssh` shell 看日志

#### B. 兜底：串口部署

保留 `deploy_serial.py`，通过 `pyserial`：

1. 打开 `/dev/ttyUSB0`
2. 自动登录 `root/luckfox`
3. 创建板端目录 `/root/ipc_lite/`
4. 将本地二进制和文本文件做 `base64`
5. 分块下发到板端临时文件
6. 板端 `base64 -d` 还原
7. 设置 `chmod +x`
8. 校验 `sha256sum`
9. 可选执行 `run.sh`

### 10.3 下发文件

第一版至少下发：

- `ipc_lite`
- `ipc_lite.ini`
- `run.sh`

如需补充共享库，再扩展清单。

## 11. 板端运行方案

统一使用：

```sh
/root/ipc_lite/run.sh
```

`run.sh` 负责：

- 设置 `LD_LIBRARY_PATH`
- 切换到脚本目录
- 确保配置文件存在
- 启动 `./ipc_lite -c ./ipc_lite.ini`

默认前台运行，方便串口直接看日志。

## 12. 验证步骤

### 12.1 主机侧

- `make -C project/app/ipc_lite`
- 确认生成 `out/ipc_lite`

### 12.2 部署侧

- 优先：

```sh
scp project/app/ipc_lite/out/ipc_lite/{ipc_lite,ipc_lite.ini,run.sh} root@<board-ip>:/root/ipc_lite/
ssh root@<board-ip> 'cd /root/ipc_lite && chmod +x ipc_lite run.sh && ./run.sh'
```

- 网络不可用时：

```sh
python3 deploy_serial.py --port /dev/ttyUSB0 --run
```

### 12.3 板端验证

检查：

- 进程启动成功
- AIQ 初始化成功
- `VI/VENC` 初始化成功
- 周期日志持续输出
- `/tmp/ipc_lite.h264` 或 `/tmp/ipc_lite.h265` 持续增长
- 发送 `Ctrl+C` 后资源能完整释放

### 12.4 RTSP 验证

开启 RTSP 时：

- 板端监听 `554`
- 主机可通过：

```sh
ffplay -rtsp_transport tcp rtsp://<board-ip>:554/live/0
```

进行拉流

## 13. 风险与已知问题

### 13.1 传感器链路仍可能报 size error

这是当前板端已有问题，不是 `ipc_lite` 独有问题。第一版会把相关错误统计清楚暴露出来，但不承诺本轮彻底修掉。

### 13.2 720p 可能不是 sensor 原生分辨率

`mis5001` 原生更高分辨率，720p 可能走 ISP/缩放链路。若后续想要低延迟高帧率，需要重新梳理：

- sensor mode
- isp path
- wrap/sharebuf
- venc buffer

### 13.3 RTMP 先留接口

当前仓库里没有现成适合直接复用的轻量 RTMP 依赖，因此第一版只保留扩展面。

### 13.4 当前板端不满足“5MP 主码流直接可用”

本轮进一步实测后确认：

- `mis5001` 驱动当前只暴露一个 mode：
  - `2592x1944`
  - `25fps`
- 使用 `simple_vi_bind_venc` 测试 `1280x720` 时，无论走：
  - `mainpath(0)`
  - `selfpath(1)`
  都会出现：
  - `mipi-csi2-hw ERR1:0x10 (fs/fe mis,vc: 0)`
  - `rkcif-mipi-lvds: ERROR: csi size err`
  - `rkisp-vir0: CIF_ISP_PIC_SIZE_ERROR`
- 使用 `2592x1944` 原生分辨率时，除上述问题外，还会进一步触发：
  - `cma_alloc failed`
  - `mpp_buffer_get invalid input`
  - `rockit_rkisp_mpibuf_done` 相关 kernel panic

因此，本轮后续调试不能再默认认为：

- 720p 路径天然稳定
- 5MP 主码流能直接作为基线

### 13.5 `rkipc` 也会踩到相同的底层约束

手动拉起 `rkipc -a /oem/usr/share/iqfiles` 后确认：

- `554` 端口可监听
- `rkipc` 当前使用的是 `mis5001` 的配置
- 主码流配置仍是：
  - `2592x1944@25`
  - `H.265`
- 子码流配置是：
  - `704x576@25`
  - `H.265`

同时日志显示：

- 主 5MP 路径同样会遇到 `CMA alloc failed`
- 底层仍有 `csi size err / PIC_SIZE_ERROR`
- 但 `rkipc` 至少还能把更小的通道继续拉起

这意味着当前阶段应把 `rkipc` 视为：

- 可用的对照样本
- 但不是“完全健康、可直接照搬”的真值源

### 13.6 当前板子的自动启动流程被人为修改过

启动日志里有：

```text
Skip rkipc autostart for Wi-Fi debugging
```

说明板子当前不是标准出厂启动路径，`RkLunch.sh` 被改成了跳过 `rkipc` 自启动。

这会影响：

- 对“之前 `rkipc` 能出视频”的复现
- 对官方工作链路的对照分析

后续在复现 `rkipc` 时，应优先使用：

- 手动启动
- 明确 ini
- 明确日志

避免把启动环境差异误判为媒体链路差异

## 14. 第二阶段建议

在第一版跑通后，再按无人机图传方向推进：

- 固化低延迟码率控制
- 评估 720p60 / 720p90 可能性
- 增加 UDP/RTP 或 WFB 方向的发送器
- 加入更细粒度的网络与链路状态上报
- 评估独立 watchdog 和自恢复策略

## 15. 本轮实施目标

本轮交付以“可运行最小系统”为准：

1. 文档落地
2. 新增独立工程
3. 本地交叉编译成功
4. 串口自动上传成功
5. 板端前台运行成功
6. 完成摄像头/编码链路实测

## 16. 本轮新增发现

### 16.1 `ipc_lite` 的 RTSP 控制面已验证通过

主机侧已能对 `ipc_lite` 和官方 sample 完成：

- `OPTIONS`
- `DESCRIBE`
- `SETUP`
- `PLAY`

其中，官方 sample 使用 `RTP/AVP/TCP interleaved` 已实测收到媒体包。

这说明：

- 局域网 RTSP 控制链路是通的
- 当前黑屏不应简单归因到 RTSP 网络层

### 16.2 `ipc_lite` 当前黑屏的主要原因仍是采集侧异常

`ipc_lite` 运行时可以看到：

- 周期统计在持续增长
- 但码率极低
- 典型 `last_len` 很小
- RTSP sink 曾持续报错

这说明当前送入 RTSP 的并不是稳定、正常的图像帧序列。

### 16.3 `mis5001` 驱动已增加一版针对性修复

本地已在：

- `sysdrv/source/kernel/drivers/media/i2c/mis5001.c`

加入一版修复思路：

- 将完整 mode 寄存器初始化从 `start_stream()` 前移到 `s_power()`
- 避免在 CSI 接收端正式开流时，sensor 仍在切 mode 寄存器

该修复尚未经过新内核重编和上板验证。

### 16.4 `ipc_lite` 已收敛到一个当前可复现的保守基线

进一步实测后，`ipc_lite` 当前最稳的起步配置不是原先假设的 `1280x720`，而是：

- `704x576`
- `25fps`
- `vi_channel=1`
- `venc_channel=1`
- `rkisp_selfpath`
- `H.265`

该路径在板端可稳定看到：

- 周期统计持续增长
- `timeouts=0`
- `errors=0`
- 文件输出持续增长

说明：

- 当前 `ipc_lite` 已经基本贴近 `rkipc` 的有效次码流路径
- 第一阶段“先找一条能稳定工作的最小路径”目标已达成

### 16.5 `RTSP + ffplay` 已验证可实际出画

宿主机用：

```sh
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://<board-ip>:554/live/0
```

对 `ipc_lite` 的 `H.265` 流做了实际拉流验证，结果是：

- `RTSP` 控制面正常
- `ffplay` 能出画
- 延迟大约在 `2s` 左右

接流开始阶段会出现一组 `HEVC` 参考帧相关报错，但后续可恢复正常播放。

结合当前配置：

- `fps=25`
- `gop=50`

可推断当前约 `2s` 的首帧/恢复延迟，与 `IDR` 周期过长高度一致。

### 16.6 `H.265` 当前仍明显优于 `H.264`

本轮在板子重启后的干净环境里，重新做了 `H.264/H.265` 对照测试，且都尽量固定在：

- `704x576`
- `25fps`
- `vi_channel=1`
- `venc_channel=1`
- `rkisp_selfpath`

观察结果：

- `H.265` 在有真实客户端拉流时，码率可爬升到约 `300~500 kbps`
- `H.265` 的 `ffplay` 实际可播放
- `H.264` 在当前链路下虽不再完全卡死，但大量输出都是极小包
- `H.264` 在文件输出和 `RTSP` 探测下，典型统计只有约 `5~20 kbps`
- `H.264` 的 `last_len` 长时间停留在很小的固定值，不能形成与 `H.265` 同等质量的有效视频流

当前最合理的判断是：

- 这不是单纯的 `RTSP` 问题
- 而是当前 `mis5001 + selfpath + 现有底层异常` 组合下，`H.264` 编码路径的可用性明显差于 `H.265`

因此，后续阶段不建议继续把主要精力投入在“让 `H.264` 成为当前默认路径”上。

### 16.7 `RTSP sink` 已做降噪处理

之前 `RTSP` 打开但没有活跃客户端时，`ipc_lite` 会在每一帧上都打印：

- `sink rtsp write failed`

现在已把该路径改成：

- 内部限频统计
- 大约每 `5s` 才打印一次告警

这样可以保留问题可见性，同时避免前台日志被完全刷爆。

### 16.8 网络部署路径已打通

本轮确认在板子接入可用 Wi-Fi 后，可以稳定使用：

- `ssh`
- `scp`

完成：

- 上传新二进制
- 上传配置文件
- 板端前台运行
- 前台查看日志

因此，后续联调应默认优先使用网络部署，串口只作为兜底手段。

## 17. 下一步 Debug 规划

### 17.1 调试目标调整

后续调试不再以“继续扩散编码方案尝试”为第一优先级，而改成：

1. 固定 `H.265 + 704x576 + selfpath` 作为当前基线
2. 在这条基线上继续降低延迟与提升稳定性
3. 再回到更高分辨率和系统级问题

### 17.2 优先路径：先贴近 `rkipc` 的小码流工作方式

优先尝试让 `ipc_lite` 贴近 `rkipc` 的次级有效通道，而不是强行使用当前直接失败的 720p 路径。

优先级如下：

1. 先验证 `rkipc` 当前小码流/次路径的稳定输出情况
2. 让 `ipc_lite` 增加更保守的默认路径：
   - 更小分辨率
   - 更低 buffer 压力
   - 更贴近 `rkipc` 的通道选择
3. 在此基础上再尝试回到 `720p`

当前这一阶段已经基本完成，后续不再优先扩展：

- `H.264` 默认路径
- 更复杂的多编码组合

而是先固定这条已经验证可用的 `H.265` 子路径。

### 17.2.1 低延迟方向的直接动作

在当前 `H.265` 基线上，下一步最直接、收益最高的尝试应是：

1. 将 `gop` 从 `50` 降到 `25`
2. 重新用 `ffplay` 实测：
   - 起播等待时间
   - 拖尾延迟
   - 丢帧恢复时间
3. 如有必要，再尝试更小的 `gop`

这样做的原因是：

- 当前 `ffplay` 可出画
- 当前约 `2s` 延迟与 `gop=50@25fps` 强相关
- 这是一个低风险、可快速验证的优化项

### 17.3 并行主线：修复系统级约束

后续需要并行处理两个系统级问题：

#### A. CMA 太小

当前 `.BoardConfig.mk` 中：

```makefile
export RK_BOOTARGS_CMA_SIZE="24M"
```

对 `2592x1944` 路径明显不够，已经实测打到：

- `cma_alloc failed`
- `mpp_buffer_get invalid input`
- kernel panic

后续计划：

1. 增大 CMA
2. 重启板子
3. 再验证 `rkipc` 主码流和 sample 5MP 路径

#### B. sensor/CSI 启动时序

当前 `mis5001` 的 `fs/fe mis + csi size err` 仍是首要异常。

后续计划：

1. 带上 `mis5001.c` 补丁重编内核
2. 上板验证是否改善：
   - `csi size err`
   - `PIC_SIZE_ERROR`
   - 初始开流稳定性

### 17.4 具体执行顺序

建议按下面顺序推进：

1. 固定当前 `H.265 + 704x576 + selfpath` 基线
2. 把 `gop` 调低，复测 `ffplay` 延迟与恢复时间
3. 保持这条基线不动，继续观察：
   - `frame losed`
   - `csi size err`
   - `PIC_SIZE_ERROR`
4. 修改 `CMA` 配置，重启验证
5. 带 `mis5001.c` 补丁重编内核，重启验证
6. 最后再回到 `720p` 和更高分辨率目标

### 17.5 当前结论

当前阶段最合理的判断是：

- `ipc_lite` 当前已经有一条可复现、可拉流的最小有效路径
- `H.265` 明显优于 `H.264`
- 当前真正的主矛盾不在 `RTSP` 控制面，也不在是否支持双编码
- 真正的主矛盾仍是：
  - `mis5001` 当前链路稳定性
  - `sensor/CSI` 启动与运行时序
  - 板端 `CMA` 资源不足
  - 更高分辨率路径尚未恢复健康

## 18. 2026-04-13 H.264 二次联调记录

### 18.1 本轮代码修正

本轮围绕 `ipc_lite` 的 `H.264 + RTSP` 路径做了三类收敛修正：

1. `run.sh`
   - 之前脚本固定追加 `-c ./ipc_lite.ini`
   - 当用户再传 `-c ./xxx.ini` 时，实际进程会带两个 `-c`
   - 这会导致板端测试时配置来源混杂
   - 现已改成：
     - 无参数时默认使用 `./ipc_lite.ini`
     - 有参数时直接透传

2. `rtsp_sink`
   - 之前把 `rtsp_tx_video()` 的任意非零返回都当失败
   - 但本仓库其余 `rkipc`/sample 路径并未这样处理
   - 现已改成仅在返回值 `< 0` 时记为失败并限频告警

3. `mpi_pipeline`
   - 补齐并显式同步 `VI/VENC` 帧率
   - 对齐一组更接近 `rkipc video.1` 的编码默认值：
     - `RcParam`
     - `RcParam2`
     - `H264/H265 qbias`
     - `H264 trans`
     - `filter`
     - `anti-ring`
     - `anti-line`
     - `lambda`

### 18.2 本轮关键现象

在板子重启后的干净环境里，固定：

- `704x576`
- `25fps`
- `vi_channel=1`
- `venc_channel=1`
- `rkisp_selfpath`
- `H.264`

做了重新验证。

早期状态：

- `ipc_lite` 的 `RTSP` 控制面正常
- `ffplay`/探针都能完成：
  - `OPTIONS`
  - `DESCRIBE`
  - `SETUP`
  - `PLAY`
- 但数据面长期只有：
  - 约 `5~6 kbps`
  - `last_len` 长时间固定在 `26/28`
- `ffplay` 表现为：
  - 能识别出 `H.264 High`
  - 但实际黑屏

补齐编码参数后的状态：

- 自定义 `RTSP/TCP` 探针可稳定接收约 `30s`
- 板端统计可持续维持在约 `120~170 kbps`
- `/tmp/ipc_lite.h264` 可持续增长到 `MB` 级
- 这说明当前 `ipc_lite` 已不再停留在“只有头信息的假流”阶段

### 18.3 仍未解决的问题

尽管 `ipc_lite` 的 `H.264` 数据面较本轮开始时已有明显改善，但真实播放器路径仍不稳定：

- `ffplay` 有过一次“几十秒内可见视频”的现象
- 随后又出现：
  - `Connection refused`
  - 板端通过串口可确认发生了整机重启

因此当前状态应描述为：

- `ipc_lite H.264` 已从“基本不可用的黑屏假流”提升到“可持续出码”
- 但“真实播放器长期稳定播放”仍未达标
- 问题已从“纯编码参数错误”收敛成：
  - `ipc_lite` 在真实播放链路下的稳定性问题
  - 外加底层 `sensor/CSI/ISP` 链路本身仍带错误背景

### 18.4 与 `rkipc` 的对照结论

本轮把板载 `rkipc` 的子码流临时改成 `H.264` 后，`/live/1` 可正常出画。

这至少说明：

- 当前板子并非完全不能输出 `H.264`
- `ipc_lite` 的 `H.264` 路径相较 `rkipc` 仍存在实现差异

但与此同时，串口与 `dmesg` 仍能稳定看到底层错误：

- `mipi-csi2-hw ERR1:0x10`
- `rkcif-mipi-lvds: ERROR: csi size err`
- `rkisp-vir0: CIF_ISP_PIC_SIZE_ERROR`

所以本轮不应得出“问题只在 `ipc_lite`”的结论，而应表述为：

- `ipc_lite` 的 `H.264` 路径需要继续贴近 `rkipc`
- 板级链路本身也并不干净

### 18.5 当前建议

后续若继续推进 `ipc_lite H.264`，建议按下面顺序收敛：

1. 固定当前 `704x576 + selfpath + H.264`
2. 优先针对“真实播放器接入时的稳定性”继续收敛
3. 用串口保留重启前最后现场，避免只依赖网络日志
4. 不再把“是否能完成 RTSP 握手”作为主要指标
5. 以：
   - `ffplay` 是否持续出画
   - 板子是否重启
   - 连续播放时长
   作为当前阶段主指标
