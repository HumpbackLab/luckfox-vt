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

- `width=1280`
- `height=720`
- `fps=25`
- `gop=50`
- `bitrate_kbps=2048`
- `codec=h264`
- `enable_aiq=true`
- `iq_dir=/oem/usr/share/iqfiles`
- `rtsp.enable=false`
- `file.enable=true`
- `file.path=/tmp/ipc_lite.h264`
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
3. 用串口上传至板子 `/root/ipc_lite/`
4. 在板子上直接运行

## 10. 串口部署方案

### 10.1 原则

必须满足：

- 不重打镜像
- 不走升级工具
- 不依赖用户手工复制粘贴

### 10.2 方案

新增 `deploy_serial.py`，通过 `pyserial`：

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

- `python3 deploy_serial.py --port /dev/ttyUSB0 --run`

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

