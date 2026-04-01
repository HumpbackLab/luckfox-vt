# RV1103 Luckfox Pico Mini 适配 mis5001 调试记录

## 目标

基于 `sysdrv/source/kernel/arch/arm/boot/dts/rv1103-luckfox-pico-ipc.dtsi`，让 RV1103 Luckfox Pico Mini 正确支持 `mis5001` MIPI 摄像头，并确认：

- 内核能识别 sensor
- media graph 正常建立
- `rkipc` 能拿到正确的 `ini`
- IQ 文件会进入最终镜像
- 板端能实际采流

## 已有源码改动

本地仓库里已经存在以下改动：

### 1. 设备树切换到 mis5001

文件：

- `sysdrv/source/kernel/arch/arm/boot/dts/rv1103-luckfox-pico-ipc.dtsi`

关键改动：

- `csi_dphy_input0` 从 `sc3336_out` 切到 `mis5001_out`
- 增加 `mis5001@31`
- `compatible = "imagedesign,mis5001"`
- I2C 地址为 `0x31`
- `reset-gpios = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>`
- `rockchip,camera-module-name = "CMK-OT2115-PC1"`
- `rockchip,camera-module-lens-name = "30IRC-F16"`

绑定链路为：

- `mis5001@31 -> rockchip-csi2-dphy0 -> rockchip-mipi-csi2 -> rkcif -> rkisp`

### 2. Mini BoardConfig 增加 mis5001 IQ 文件

文件：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

关键改动：

```makefile
export RK_CAMERA_SENSOR_IQFILES="sc4336_OT01_40IRC_F16.json sc3336_CMK-OT2119-PC1_30IRC-F16.json mis5001_CMK-OT2115-PC1_30IRC-F16.json"
```

### 3. RV1103 的 `RkLunch.sh` 增加 mis5001 选择逻辑

文件：

- `project/app/rkipc/rkipc/src/rv1103_ipc/RkLunch.sh`

关键改动：

```sh
if [ ! -f "/oem/usr/share/rkipc.ini" ]; then
	lsmod | grep mis5001
	if [ $? -eq 0 ]; then
		ln -s -f /oem/usr/share/rkipc-mis5001-500w.ini $default_rkipc_ini
	fi
	...
fi
```

### 4. RV1103 的 `CMakeLists.txt` 安装 mis5001 ini

文件：

- `project/app/rkipc/rkipc/src/rv1103_ipc/CMakeLists.txt`

关键改动：

```cmake
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../rv1106_ipc/rkipc-500w.ini DESTINATION share)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../rv1106_ipc/rkipc-mis5001-500w.ini DESTINATION share)
```

### 5. 修正 mis5001 驱动误导性日志

文件：

- `sysdrv/source/kernel/drivers/media/i2c/mis5001.c`

修正内容：

- 将 `Detected mis4001 ... sensor` 改成 `Detected mis5001 ... sensor`

说明：

- 这不影响功能
- 但会直接误导串口排查结果

## 板型澄清

本轮排查后确认，实际硬件始终是：

- `RV1103_Luckfox_Pico_Mini`

需要特别说明的是：

- `rv1103g-luckfox-pico-mini.dts`
- `rv1103g-luckfox-pico.dts`

都引用了同一个：

- `rv1103-luckfox-pico-ipc.dtsi`

因此：

- `mis5001` 的设备树改动本身已经同时作用于 `Pico` 和 `Pico Mini`
- 真正需要按板型区分的是 `BoardConfig`
- `Mini` 板型必须保留自己的 Wi-Fi 配置与 overlay

## 2026-04-01 板端实测

通过 `ttyUSB0` 串口登录开发板，账号密码为 `root/luckfox`。

### 1. 串口登录正常

实测可正常进入 shell。

### 2. 内核已识别 mis5001

板端 `dmesg` 看到：

- `mis5001 4-0031: driver version: 00.01.02`
- `rockchip-csi2-dphy csi2-dphy0: dphy0 matches m00_b_mis5001 4-0031:bus type 5`

同时还看到了一个误导性旧日志：

- `Detected mis4001 1311 sensor`

这就是上面修正 `mis5001.c` 日志字符串的原因。

### 3. media graph 已建立

板端存在：

- `/dev/media0`
- `/dev/media1`
- `/dev/video0` 到 `/dev/video20`

`media-ctl -p` 可见：

- sensor 节点 `m00_b_mis5001 4-0031`
- `rockchip-csi2-dphy0`
- `rockchip-mipi-csi2`
- `rkcif-mipi-lvds`
- `rkisp-isp-subdev`
- `rkisp_mainpath`

说明：

- 内核链路不是“完全没起来”
- `mis5001 -> CSI -> CIF -> ISP` 已经连通

### 4. 板端缺少 mis5001 用户态配置

板端实测时：

- 没有 `rkipc` 进程
- 没有 `rkaiq` 进程
- `/oem/usr/share/rkipc.ini` 不存在
- `/oem/usr/share` 只有：
  - `rkipc-300w.ini`
  - `rkipc-400w.ini`
- 没找到：
  - `rkipc-500w.ini`
  - `rkipc-mis5001-500w.ini`
  - `mis5001_CMK-OT2115-PC1_30IRC-F16.json`

这说明板端镜像里的用户态资源并不完整。

### 5. 板端尝试抓帧时出现 CSI 错误

执行：

```sh
v4l2-ctl -d /dev/video11 \
  --set-fmt-video=width=640,height=480,pixelformat=NV12 \
  --stream-mmap=4 \
  --stream-count=1 \
  --stream-to=/tmp/cam_nv12.bin
```

现象：

- `rkcif-mipi-lvds: stream[0] start streaming`
- `rockchip-mipi-csi2 ... stream ON`
- 生成了 `/tmp/cam_nv12.bin`

同时内核报错：

- `mipi-csi2-hw ERR1:0x1000000 (crc,vc: 0)`
- `mipi-csi2-hw ERR1:0x10 (fs/fe mis,vc: 0)`
- `rkcif-mipi-lvds: ERROR: csi size err`
- `rkisp-vir0: CIF_ISP_PIC_SIZE_ERROR`

结论：

- 不是完全无法采流
- 但当前采流仍然不稳定，存在 CSI/时序/数据格式层面的错误

## 本地 buildroot 仓库排查过程

### 1. 先核对源码是否真的改了

确认：

- `rv1103-luckfox-pico-ipc.dtsi` 已切到 `mis5001`
- `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico-IPC.mk` 已加 `mis5001` IQ
- `rv1103_ipc/RkLunch.sh` 已加 `mis5001` 逻辑
- `rv1103_ipc/CMakeLists.txt` 已加 `rkipc-500w.ini` 和 `rkipc-mis5001-500w.ini`

### 2. 再核对打包链路

`project/build.sh` 的资源打包逻辑表明：

- `app_out/share/*` 会被拷到最终 `oem/usr/share`
- `media_out/isp_iqfiles/$RK_CAMERA_SENSOR_IQFILES` 会被拷到 `oem/usr/share/iqfiles`

也就是说，源码路径本身没有走错。

### 3. 发现本地默认 BoardConfig 一度切到了错误目标

在本轮中途，本地 `.BoardConfig.mk` 曾被切到：

- `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico-IPC.mk`

但实际硬件始终是：

- `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

这会导致：

- 构建时使用错误的板级配置
- Wi-Fi 配置和 `overlay-luckfox-wifibt-firmware` 丢失
- 产物与真实硬件板型不一致

### 4. 发现 `output/out` 确实是旧产物

排查旧产物时：

- `output/out/app_out/share` 只有
  - `rkipc-300w.ini`
  - `rkipc-400w.ini`
- `output/out/oem/usr/share/iqfiles` 只有
  - `sc4336`
  - `sc3336`

虽然 `output/out/media_out/isp_iqfiles` 里已经有：

- `mis5001_CMK-OT2115-PC1_30IRC-F16.json`

但它没有被带进最终 `oem`。

这说明问题不是“源码没写”，而是：

- 默认板型选错了
- `app_out/oem` 还是旧产物

### 5. 切回正确的 Mini BoardConfig

本地将 `.BoardConfig.mk` 切到：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

并在该配置中补充：

- `mis5001_CMK-OT2115-PC1_30IRC-F16.json`

然后执行：

```sh
./build.sh clean app
./build.sh app
./build.sh firmware
```

### 6. 重建后验证通过

重建后确认：

- `output/out/app_out/share/`
  - `rkipc-300w.ini`
  - `rkipc-400w.ini`
  - `rkipc-500w.ini`
  - `rkipc-mis5001-500w.ini`

- `output/out/oem/usr/share/`
  - `rkipc-300w.ini`
  - `rkipc-400w.ini`
  - `rkipc-500w.ini`
  - `rkipc-mis5001-500w.ini`

- `output/out/oem/usr/share/iqfiles/`
  - `mis5001_CMK-OT2115-PC1_30IRC-F16.json`

- 新镜像已生成：
  - `output/image/oem.img`
  - `output/image/rootfs.img`
  - `output/image/update.img`

## 本轮定位到的根因

### 已确认根因 1

板端缺少 `mis5001` 的 `ini` 和 IQ 文件，根因不是仓库源码缺改动，而是：

- 之前的 `Mini` 板型构建没有把 `mis5001` IQ 加进 `RK_CAMERA_SENSOR_IQFILES`
- 中途一度把默认 `.BoardConfig.mk` 切到了错误的 `Pico` 配置
- 之前的 `output/out/app_out` 和 `output/out/oem` 是旧产物
- 板子大概率烧录的也是旧镜像

### 已确认根因 2

`mis5001` 驱动成功识别后打印了错误的 sensor 名称：

- `Detected mis4001 ...`

这会误导排查，现已在源码里修正为：

- `Detected mis5001 ...`

### 尚未完全解决的问题

内核已经能把链路拉起来，但抓帧时仍出现：

- CRC 错误
- frame start / frame end mismatch
- CSI size error
- `CIF_ISP_PIC_SIZE_ERROR`

这部分说明：

- 用户态资源缺失并不是唯一问题
- 即使补齐 `ini`/IQ，底层 MIPI 采流稳定性仍需继续验证

## 对 CSI 错误的当前判断

当前只能做出以下谨慎结论：

### 已确认

- `mis5001` 驱动已加载
- sensor 已挂到 media graph
- `supported_modes[]` 只有一个模式：
  - `2592x1944`
  - `RAW10`
  - `2lane`
  - `891Mbps`
- `rkipc-mis5001-500w.ini` 分辨率与驱动模式一致，为 `2592x1944`

### 仍需继续验证

- 当前模组是否真的与仓库 `mis5001` 寄存器表完全匹配
- 当前 sensor 输出时序是否与驱动里的 `891Mbps` 配置一致
- 实际板级 reset/power 时序是否足够稳定
- 是否还需要进一步校准 sensor 寄存器表或 lane/timing

换句话说：

- 用户态打包问题已经定位并在本地产物中修正
- 采流层面的 `crc/size err` 仍是后续重点

## 当前结论

截至 2026-04-01，本次 `mis5001` 适配的状态应更新为：

- 源码侧：
  - 共享 `DTSI`
  - `Mini BoardConfig`
  - `RkLunch.sh`
  - `CMakeLists.txt`
  已具备 `mis5001` 支持

- 构建侧：
  - 只要使用正确的 `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`
  - 并重新执行 `./build.sh app` 与 `./build.sh firmware`
  - `rkipc-mis5001-500w.ini` 和 `mis5001` IQ 文件就能进入最终镜像
- 同时不会丢掉 `RTL8812AU` 的 Wi-Fi 配置

- 板端侧：
  - 旧镜像缺少 `mis5001` 相关用户态资源
  - 新镜像需要重新烧录验证

- 运行态：
  - 内核能识别 `mis5001`
  - media graph 已建立
  - 烧录新镜像后，`rkipc` 已能按 `mis5001` 的 `ini` 和 IQ 正常启动
  - 并且已经成功抓到一帧原始图像

## 烧录新镜像后的二次实测

在生成并烧录新的 `update.img` 之后，再次通过 `ttyUSB0` 串口复查。

### 1. `rkipc` 已使用 mis5001 配置启动

开机日志中可见：

- `rkipc -a /oem/usr/share/iqfiles`
- `sensor_name is m00_b_mis5001 4-0031`
- `rk_aiq_uapi_sysctl_init success. iq:/oem/usr/share/iqfiles/mis5001_CMK-OT2115-PC1_30IRC-F16.json`

同时板端确认存在：

- `/oem/usr/share/rkipc-500w.ini`
- `/oem/usr/share/rkipc-mis5001-500w.ini`

并且校验发现：

- `/userdata/rkipc.ini`

与：

- `/oem/usr/share/rkipc-mis5001-500w.ini`

的 `md5` 完全一致，说明启动脚本已经正确选择了 `mis5001` 专用配置。

### 2. 驱动日志修正已在板端体现

新的板端日志中已经是：

- `Detected mis5001 1311 sensor`

不再是之前误导排查的：

- `Detected mis4001 ...`

### 3. 重新检查后，未再看到首次实测中的 CSI 报错

烧录后重新开机并启动 `rkipc` 时，未再复现之前首次实测中的：

- `crc`
- `fs/fe mis`
- `csi size err`
- `CIF_ISP_PIC_SIZE_ERROR`

至少在本轮启动路径中，链路稳定性比旧镜像有明显改善。

## 图像抓取验证

### 1. 先尝试走 `rkipc` 自带 JPEG 抓拍

源码确认 `rv1103_ipc` 版本的 `rkipc` 具备 JPEG 抓拍能力：

- `project/app/rkipc/rkipc/src/rv1103_ipc/video/video.c`

其中：

- `rk_take_photo()`
- `rkipc_get_jpeg()`
- `video.jpeg:*`

共同负责 JPEG 抓拍。

因此先尝试通过以下两条路径验证 JPEG 抓拍：

- 本地 Unix socket `/var/tmp/rkipc` 调 `rk_take_photo`
- 启用 `video.jpeg:enable_cycle_snapshot = 1`

### 2. JPEG 抓拍仍然失败

实测时，JPEG 抓拍日志持续出现：

- `RK_MPI_VENC_GetStream timeout a004800e`
- `the last photo was not completed`

进一步查看 `rv1103_ipc/video/video.c` 可见，对于 RV1103：

```c
LOG_INFO("1103 combo, jpeg resolution must be consistent with the main stream resolution\n");
```

于是又临时把：

- `video.jpeg:width`
- `video.jpeg:height`
- `video.jpeg:jpeg_buffer_size`

调整到和主码流一致，但 JPEG 抓拍依然没有成功落图。

同时在一次手工重启 `rkipc` 的试验中，还看到：

- `rkipc_pipe_0_init: ERROR: create VI error! ret=-1`

说明直接通过用户态反复重启 `rkipc` 做 JPEG 验证，容易把 VI 初始化状态弄脏。

因此本轮结论是：

- `mis5001` 主视频链路已经能跑起来
- 但 `rkipc` 的 JPEG 抓拍通路目前仍未完全打通

### 3. 改为直接抓原始帧验证

为了绕开 JPEG 抓拍链路，改为：

- 停止 `rkipc`
- 直接从 `/dev/video11` 抓取一帧原始 `NV12`

执行思路：

- 独占 `rkisp_mainpath`
- 设置 `640x480 NV12`
- 取 1 帧到板端临时文件

板端抓取得到：

- `/tmp/cam_640x480.nv12`

文件大小确认：

- `460800` 字节

板端 `sha256` 为：

- `6399b3c06b61deee9195d4d86c7704e9041389859d7514e5f6778673315f2e19`

### 4. 通过串口小包分片回传到当前工作区

由于直接通过控制台一次性回传整帧数据不稳定，最终采用：

- 串口小包分片回传
- 每包固定较小数据量
- 本机逐片重组并校验

本机重组后的文件为：

- `capture-640x480.nv12`

重组后校验：

- 文件大小仍为 `460800`
- `sha256` 与板端完全一致：
  - `6399b3c06b61deee9195d4d86c7704e9041389859d7514e5f6778673315f2e19`

说明：

- 串口小包回传的数据完整无损

### 5. 在本地将 NV12 转换为 PNG

在宿主机安装 `Pillow` 后，将回传的原始帧转换为：

- `capture-640x480.png`

最终人工查看该图片，画面正常。

这意味着：

- `mis5001 -> CSI -> CIF -> ISP -> video11`

这一条原始采图链路已经实测跑通。

## 对当前状态的更新结论

截至本轮验证，可以把结论从“仅完成基础适配”更新为：

- `mis5001` 在 `RV1103_Luckfox_Pico_Mini` 上：
  - 驱动识别正常
  - IQ 文件加载正常
  - `rkipc` 专用配置选择正常
  - 原始图像抓取正常

但仍需保留一个未完成项：

- `rkipc` 的 JPEG 抓拍路径仍然有 `VENC GetStream timeout`，尚未完全修通

## 下一步建议

### 1. 烧录本地最新镜像

优先烧录：

- `output/image/update.img`

### 2. 上板复核用户态资源

烧录后先确认：

```sh
ls -l /oem/usr/share/rkipc*.ini
find /oem/usr/share/iqfiles -maxdepth 1 -iname '*mis5001*'
```

### 3. 再看 `rkipc` 是否能正常拉起

建议确认：

```sh
ps -ef | grep rkipc
ls -l /oem/usr/share/rkipc.ini
```

### 4. 若仍有 `crc/size err`，继续查底层

重点继续查：

- sensor 寄存器表
- MIPI timing / data rate
- reset/power 时序
- 实际模组型号与 `mis5001.c` 的匹配程度
