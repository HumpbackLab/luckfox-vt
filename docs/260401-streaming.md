# mis5001 经 H.265 后通过 Wi-Fi 发到客户端的实现路径

## 结论

基于当前仓库，最直接、改动最小、且已经基本具备条件的实现方式是：

1. 板端用 `mis5001 + rkipc` 采集并硬件编码为 `H.265`
2. 板端通过 `wlan0` 接入同一个 Wi-Fi 网络
3. 板端启动 `RTSP` 服务
4. 客户端电脑通过 `rtsp://<board-ip>/live/0` 或 `live/1` 拉流

也就是说，当前仓库更接近“板端提供 RTSP 服务，客户端拉流”，而不是“板端主动推送到远端播放器”。

如果“推到客户端”只是指“通过 Wi-Fi 把码流送到另一台电脑看”，那么当前仓库已经基本走通这条链路，不需要再单独做一套新的采集/编码/发送程序。

## 当前仓库已经具备的能力

### 1. `mis5001` 已接入 RV1103 的启动链

- `project/app/rkipc/rkipc/src/rv1103_ipc/RkLunch.sh`
  已根据 `lsmod | grep mis5001` 选择 `/oem/usr/share/rkipc-mis5001-500w.ini`
- `project/app/rkipc/rkipc/src/rv1103_ipc/CMakeLists.txt`
  已把 `../rv1106_ipc/rkipc-mis5001-500w.ini` 安装进镜像
- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`
  已把 `mis5001_CMK-OT2115-PC1_30IRC-F16.json` 加入 `RK_CAMERA_SENSOR_IQFILES`

这说明：

- `RV1103_Luckfox_Pico_Mini` 的 `mis5001` 不是只停在 DTS 层
- `rkipc` 的默认配置选择和 IQ 文件打包链路也已经接上了

### 2. `mis5001` 默认配置就是 H.265 + RTSP

`project/app/rkipc/rkipc/src/rv1106_ipc/rkipc-mis5001-500w.ini` 当前关键配置是：

- `enable_rtsp = 1`
- `enable_rtmp = 0`
- 主码流 `video.0` 为 `2592x1944@25fps`
- 子码流 `video.1` 为 `704x576@25fps`
- 两路 `output_data_type = H.265`

因此就默认行为来说：

- RTSP 已开启
- RTMP 默认关闭
- 编码类型已经是 H.265

### 3. `rkipc` 代码里已经按 `output_data_type` 配 H.265 编码器

`project/app/rkipc/rkipc/src/rv1106_ipc/video/video.c` 中：

- `video.0:output_data_type = H.265` 时，主码流会走 `RK_VIDEO_ID_HEVC`
- `video.1:output_data_type = H.265` 时，子码流也会走 `RK_VIDEO_ID_HEVC`

说明当前不是“INI 写了 H.265 但代码没接”，而是参数和 VENC 初始化是一致的。

### 4. RTSP 服务已经在 `554` 端口监听

`project/app/rkipc/rkipc/common/rtsp/rtsp.c` 中：

- `create_rtsp_demo(554)`
- `video.0` 默认映射到 `/live/0`
- `video.1` 默认映射到 `/live/1`

`project/app/rkipc/rkipc/src/rv1106_ipc/video/video.c` 中：

- 初始化时如果 `enable_rtsp` 为真，就调用 `rkipc_rtsp_init(RTSP_URL_0, RTSP_URL_1, NULL)`

因此板端拿到 IP 后，客户端直接拉：

- `rtsp://<board-ip>/live/0`
- `rtsp://<board-ip>/live/1`

即可。

### 5. Wi-Fi 接入链也已经具备

当前默认的 `RV1103_Luckfox_Pico_Mini` 板级配置：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

已经设置：

- `RK_ENABLE_WIFI=y`
- `RK_ENABLE_WIFI_CHIP=AR9271`
- `LF_WIFI_SSID`
- `LF_WIFI_PSK`

`project/build.sh` 在 `RK_ENABLE_WIFI=y` 时会自动生成：

- `project/app/wifi_app/wpa_supplicant.conf`

`sysdrv/drv_ko/wifi/insmod_wifi.sh` 当前也已经：

- 支持识别 `AR9271`
- 对 `ath9k_htc.ko` 使用 `nohwcrypt=1`
- 在发现 `wlan0` 后自动启动 `wpa_supplicant`
- 再用 `dhcpcd` 或 `udhcpc` 获取 IP

这意味着当前仓库并不缺“联网基础能力”。

## 推荐实现方式

### 方案 A：直接用现有 `rkipc + RTSP`，客户端拉流

这是当前仓库最适合的方案。

数据路径是：

`mis5001 -> CSI/CIF/ISP -> rkaiq -> rkipc -> VENC(H.265) -> RTSP(554) -> Wi-Fi -> PC 客户端`

优点：

- 不需要再写新的采集程序
- 不需要额外引入 GStreamer/FFmpeg 推流程序
- 直接复用现有 `rkipc` 启动链
- 现有 `mis5001` 专用 ini 已经是 H.265

客户端电脑测试方式：

```sh
ffplay -rtsp_transport tcp rtsp://<board-ip>/live/0
```

子码流：

```sh
ffplay -rtsp_transport tcp rtsp://<board-ip>/live/1
```

### 什么时候不建议现在就做“主动推送”

如果你说的“推到客户端”严格指：

- 板端主动连到电脑
- 电脑只被动接收

那么当前仓库默认链路并不是这个模式。

当前代码里虽有 `RTMP` 发送路径，但它的 URL 在代码中是硬编码到：

- `rtmp://127.0.0.1:1935/live/mainstream`
- `rtmp://127.0.0.1:1935/live/substream`

而且 `rkipc-mis5001-500w.ini` 里当前还是：

- `enable_rtmp = 0`

所以“远端电脑主动作为推流接收端”这条线，在当前仓库里不是现成默认能力。

## 上板落地步骤

### 1. 选择当前目标板级

如果目标硬件是 `RV1103 Luckfox Pico Mini`，应使用：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

### 2. 构建镜像

```sh
./build.sh lunch
./build.sh
```

在 `lunch` 时选择：

- `BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

### 3. 烧录并启动后确认板端状态

先确认相机：

```sh
dmesg | grep -i mis5001
```

再确认 `rkipc`：

```sh
ps | grep rkipc
```

确认 Wi-Fi：

```sh
ip addr show wlan0
```

确认 RTSP 端口：

```sh
netstat -lntp | grep 554
```

### 4. 在客户端电脑播放

假设板端无线 IP 是 `192.168.1.50`：

```sh
ffplay -rtsp_transport tcp rtsp://192.168.1.50/live/0
```

## 如果必须改成“板端主动推送到电脑”

建议分成两种情况。

### 方案 B1：改成推送到电脑上的 RTMP 服务器

这是最接近当前代码结构的做法。

需要改：

1. 把 `project/app/rkipc/rkipc/src/rv1103_ipc/video/video.c`
   或 `rv1106_ipc/video/video.c` 里的 `RTMP_URL_0/1`
   从 `127.0.0.1` 改成电脑 IP 或域名
2. 把 `rkipc-mis5001-500w.ini` 里的 `enable_rtmp` 改成 `1`
3. 电脑上运行 RTMP 服务端
   例如 `mediamtx` 或 `nginx-rtmp`

优点：

- 仍然复用当前 `rkipc` 编码链
- 改动范围小

缺点：

- 需要电脑侧先跑 RTMP 服务
- 当前 URL 是编译时硬编码，最好进一步改成从 `ini` 读取

### 方案 B2：直接增加 RTP/UDP 发送

这是更偏定制开发的方案。

思路是：

- 在 `rkipc_rtsp_write_video_frame()` 同级位置
- 或 VENC 出帧线程里
- 直接把 H.265 Annex-B 码流封装成 RTP/UDP 发给电脑

这条线能实现真正“板端主动推到电脑”，但当前仓库没有现成的通用实现，工作量会比 RTSP/RTMP 方案大。

## 最终建议

对当前仓库，优先级建议是：

1. 先按现有 `mis5001 + H.265 + RTSP + Wi-Fi` 跑通
2. 用电脑 `ffplay` / `VLC` 验证主码流和子码流
3. 如果业务上必须“板端主动推送”，再把现有 RTMP 路径参数化并改到远端电脑

也就是说，当前仓库距离目标最近的答案不是“新写一套发送程序”，而是：

- 直接使用现有 `rkipc`
- 让板子通过 Wi-Fi 联网
- 客户端从 RTSP 地址拉取 H.265 码流
