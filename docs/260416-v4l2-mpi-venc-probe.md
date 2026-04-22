# v4l2_mpi_venc_probe 零拷贝验证记录

更新：2026-04-22 当前正式高帧率配置已切到 `1280x720@90` 和
`1920x1080@60`。本文中 `1104x624@120` 相关命令只作为历史 probe
验证记录保留，对应 ini 已从当前 `ipc_lite` 配置集中移除。

日期：2026-04-16

## 1. 目的

`v4l2_mpi_venc_probe` 是今天新增的诊断工具，用来验证一条绕过 `ipc_lite`
主流程的低延迟路径：

```text
/dev/video12 V4L2 selfpath
  -> MPI MMZ dma-buf
  -> RK_MPI_VENC_SendFrame
  -> UDP RTP
```

最终保留目标是：

- 只保留零拷贝通路
- 不再保留 `mmap-copy` / memcpy 对照通路
- 用它对比 `ipc_lite` 默认 `VI -> VENC bind` 路径的板端延迟
- 继续复用 `ipc_lite.720p60.fast.udp.ini` 的编码和 UDP RTP 配置

## 2. 当前默认行为

当前 `v4l2_mpi_venc_probe` 默认就是零拷贝：

```text
V4L2_MEMORY_DMABUF
RK_MPI_MMZ_Alloc
RK_MPI_MB_Handle2Fd
VIDIOC_QBUF(fd)
VIDIOC_DQBUF
RK_MPI_VENC_SendFrame
```

固定像素格式：

```text
V4L2_PIX_FMT_NV12 + RK_FMT_YUV420SP
```

## 3. 常用命令

### 3.1 板端连续运行

在板子 `/root/ipc_lite` 下：

```sh
cd /root/ipc_lite
./v4l2_mpi_venc_probe -c ./ipc_lite.720p60.fast.udp.ini -d /dev/video12 -n 0 -b 2 --drain
```

参数含义：

- `-c ./ipc_lite.720p60.fast.udp.ini`
  - 读取 720p60 H.264 UDP RTP 配置
- `-d /dev/video12`
  - 使用 `rkisp_selfpath`
- `-n 0`
  - 持续运行，直到 `Ctrl-C` 或 `SIGTERM`
- `-b 2`
  - V4L2/MPI buffer 数量为 2
- `--drain`
  - 每次取到一帧后，继续用非阻塞方式 drain 队列，只保留最新帧

### 3.2 短跑 60 帧

```sh
cd /root/ipc_lite
./v4l2_mpi_venc_probe -c ./ipc_lite.720p60.fast.udp.ini -d /dev/video12 -n 60 -b 2 --drain
```

用于快速确认：

- ISP 是否正常启动
- V4L2 是否协商到预期格式
- VENC 是否正常输出
- UDP RTP 是否正常发送
- 板端各阶段耗时是否异常

## 4. 输出指标

典型日志：

```text
v4l2_mpi_venc_probe dev=/dev/video12 1280x720 fps=60 frames=60 buffers=2
drain=on mode=mpi-dmabuf format=NV12 negotiated=NV12 bytesperline=1280 frame_size=1382400

frame=60 seq=60 age=13.871ms send=5.012ms sync=0.822ms get=11.037ms sink=24.344ms total_since_v4l2_ts=25.362ms len=8067 key=0 drained=0

summary frames=60 bytes=418165 drained=0 mode=mpi-dmabuf
age=14.409/32.013ms wait=7.192/15.120ms buffer_setup=2.208/2.423ms
sync=0.396/0.822ms send=0.589/5.012ms get=5.743/11.037ms
sink=2.797/24.344ms total_since_v4l2_ts=24.032/63.228ms
```

字段含义：

- `age`
  - `VIDIOC_DQBUF` 完成时间减去 V4L2 buffer timestamp
- `wait`
  - 等待并取到 V4L2 frame 的耗时
- `buffer_setup`
  - 启动阶段分配 MPI MMZ buffer、设置 stride、导出 fd 的耗时
- `sync`
  - V4L2 写完 dma-buf 后，送 VENC 前的 cache/MMZ 同步耗时
- `send`
  - `RK_MPI_VENC_SendFrame` 耗时
- `get`
  - `RK_MPI_VENC_GetStream` 耗时
- `sink`
  - UDP RTP sink 写出耗时
- `total_since_v4l2_ts`
  - 从 V4L2 timestamp 到本轮 UDP RTP 写出后的总耗时
- `drained`
  - `--drain` 丢弃的旧帧数量

## 5. 今天的关键结论

### 5.1 `-n 0` 的语义修正

早期代码里 `-n 0` 会被重新改成默认 `300` 帧，导致 60fps 下大约 5 秒退出。

现在语义已修正：

```text
-n 0 means run until SIGINT/SIGTERM
```

### 5.2 必须启动 ISP/AIQ

最初 probe 直接打开 `/dev/video12`，没有启动 `ipc_lite_isp_start()`。

当时 raw buffer 采样显示：

```text
Y=8.0[8,8] U=128.0[128,128] V=128.0[128,128]
```

也就是 V4L2 拿到的是固定空帧。QGC 看到的绿画面不是接收端问题，而是 probe 没有把 ISP 正常拉起来。

当前 probe 已和主程序一样先启动：

```text
ipc_lite_isp_start()
```

然后再打开 V4L2 / 初始化 VENC。

### 5.3 零拷贝路径最终可用

尝试过：

- 固定 `NV12`
- cache flush
- `mmap-copy`
- MPI MMZ uncached buffer
- 启动 ISP/AIQ

最终确认：

- 关键不是 `mmap-copy`
- 关键是 probe 也要启动 ISP/AIQ
- 当前 `mpi-dmabuf` 零拷贝路径已经可以正常出图

因此 `mmap-copy` 对照通路已删除，只保留零拷贝。

### 5.4 当前板端耗时量级

短跑 60 帧时，零拷贝路径典型结果：

```text
age avg ~= 14ms
sync avg < 1ms
send avg < 1ms
get avg ~= 5-6ms
total_since_v4l2_ts avg ~= 24ms
```

首帧或个别帧会有较大的 `sink` / `total` 尖峰，不应直接当成稳态平均值。

### 5.5 1104x624@120 编码链路确认

2026-04-19 在 `192.168.3.50` 上使用 MIS5001 原生
`1104x624@120` sensor mode，`/dev/video12` 输出 NV12，经
`v4l2_mpi_venc_probe` 送 MPI VENC 编码 H.264。

测试配置：

```sh
./v4l2_mpi_venc_probe \
  -c ./ipc_lite.1104x624p120.probe.ini \
  -d /dev/video12 \
  -n 1200 \
  -b 4
```

该配置关闭 UDP RTP 输出，只验证 V4L2 capture + MPI VENC 编码吞吐。

结果：

```text
summary sent=1200 streams=1200 bytes=10203768 drained=0 mode=mpi-dmabuf threaded=yes elapsed=9.996s capture_fps=120.05 stream_fps=120.05 age=8.894/13.289ms wait=6.803/13.331ms buffer_setup=1.654/1.960ms sync=0.274/1.001ms send=1.070/6.264ms get=8.316/200.438ms sink=0.003/0.012ms total_since_v4l2_ts=15.152/31.127ms
```

结论：

- 1104x624@120 下，V4L2 + MPI VENC 本身可以达到 120fps。
- 1200 帧测试中 `sent` 与 `streams` 相等，没有用户态可见的编码掉帧。
- dmesg 只有起流瞬间的 CSI 同步/CRC 报文，没有持续的 `PIC_SIZE_ERROR`
  或 VENC 失败。
- 该结论不覆盖 UDP/RTP 发送端或接收端吞吐，网络输出需要单独测试。

UDP RTP 打开后的对照测试：

```sh
./v4l2_mpi_venc_probe \
  -c ./ipc_lite.1104x624p120.fast.udp.ini \
  -d /dev/video12 \
  -n 1200 \
  -b 4
```

发往当前主机 `192.168.3.51:5600` 时，1200 帧测试结果：

```text
summary sent=1200 streams=853 bytes=7330974 drained=0 mode=mpi-dmabuf threaded=yes elapsed=9.999s capture_fps=120.01 stream_fps=85.31 age=9.291/17.880ms wait=6.302/15.546ms buffer_setup=1.159/1.261ms sync=0.291/0.593ms send=1.574/11.182ms get=1.735/200.543ms sink=9.612/311.727ms total_since_v4l2_ts=1152.846/1379.852ms
```

此时 sensor/V4L2/VENC capture 仍是 120fps，但 UDP sink 写 socket
平均 9.6ms，最大 311ms，stream 线程只能到 85fps。

临时把同一配置的 UDP 目标改为 `127.0.0.1:5600`，600 帧结果：

```text
summary sent=600 streams=600 bytes=5570169 drained=0 mode=mpi-dmabuf threaded=yes elapsed=5.006s capture_fps=119.85 stream_fps=119.85 age=8.982/14.891ms wait=6.804/14.854ms buffer_setup=1.631/2.995ms sync=0.304/9.010ms send=1.050/4.989ms get=7.167/201.282ms sink=0.835/47.572ms total_since_v4l2_ts=17.510/74.372ms
```

因此 UDP 打开后真实发到主机不能达到 120fps，瓶颈在板端 wlan0 到主机的
UDP 发包路径；loopback UDP 可以达到 120fps。

为避免 UDP 慢时发送旧帧，`v4l2_mpi_venc_probe` 在 UDP sink 启用时会在
每次写 socket 前丢弃 VENC 输出队列中已积压的旧编码帧，只发送本轮可见的
最新编码帧。该策略不追随后续新产生的帧，避免 drain 循环饿死 UDP 发送。

2026-04-19 使用该策略发往 `192.168.3.51:5600`，600 帧测试结果：

```text
summary sent=600 streams=274 bytes=2171324 drained=0 stream_dropped=326 mode=mpi-dmabuf threaded=yes elapsed=5.002s capture_fps=119.95 stream_fps=54.78 age=9.167/18.440ms wait=6.949/16.314ms buffer_setup=1.144/1.248ms sync=0.288/0.587ms send=0.904/7.824ms get=3.310/201.420ms sink=11.277/83.998ms total_since_v4l2_ts=27.686/103.737ms
```

这里的 `stream_fps` 是实际送进 UDP sink 的帧率；`stream_dropped` 是被丢弃
的旧编码帧数量。capture/VENC 输入仍保持 120fps，UDP 输出保持最新帧优先，
端到端积压从秒级降到几十毫秒级。

## 6. 注意事项

1. 运行前要确保没有旧的 `ipc_lite` 或 probe 占用 media/V4L2/VENC。

2. 如果板子重启后 sensor mode 回到默认高分辨率，需要先通过 `run.sh` 或 `media-ctl` 把 sensor 切回 `1280x720`。

3. 当前工具只支持 H.264：

```text
v4l2_mpi_venc_probe currently supports h264 only
```

4. `RKSockServer accept failed` 常出现在退出清理阶段，目前不是判断 probe 失败的核心依据。

5. `--drain` 当前多数情况下 `drained=0`，说明用户态可见的 V4L2 队列里通常没有明显旧帧堆积。

## 7. 构建与部署

本地构建：

```sh
export PATH=/home/ncer/luckfox-vt/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
make -C project/app/ipc_lite/build v4l2_mpi_venc_probe
```

上传到板子：

```sh
scp project/app/ipc_lite/build/v4l2_mpi_venc_probe root@10.9.6.195:/root/ipc_lite/v4l2_mpi_venc_probe
```

板端运行：

```sh
ssh root@10.9.6.195
cd /root/ipc_lite
chmod +x v4l2_mpi_venc_probe
export LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib:${LD_LIBRARY_PATH:-}
./v4l2_mpi_venc_probe -c ./ipc_lite.720p60.fast.udp.ini -d /dev/video12 -n 0 -b 2 --drain
```

1104x624@120 编码吞吐验证：

```sh
./v4l2_mpi_venc_probe -c ./ipc_lite.1104x624p120.probe.ini -d /dev/video12 -n 1200 -b 4
```

1104x624@120 UDP RTP 输出验证：

```sh
./v4l2_mpi_venc_probe -c ./ipc_lite.1104x624p120.fast.udp.ini -d /dev/video12 -n 1200 -b 4
```
