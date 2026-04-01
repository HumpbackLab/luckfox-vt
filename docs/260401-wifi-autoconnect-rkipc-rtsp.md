# Wi-Fi 自动连接与 RKIPC/RTSP 验证记录

## 目标

本轮工作的目标包括：

- 让镜像默认携带指定 Wi-Fi 配置
- 保证板端尽量只使用一份有效 Wi-Fi 配置
- 验证 `AR9271` 网卡可以稳定连上 `WPA2-PSK` 网络
- 确认 `rkipc` 的启动链
- 确认板端 IP 视频流的访问方式

## 默认 Wi-Fi 配置

### 1. 构建期配置入口

`project/build.sh` 在 `RK_ENABLE_WIFI=y` 时，会根据：

- `LF_WIFI_SSID`
- `LF_WIFI_PSK`

自动生成 `project/app/wifi_app/wpa_supplicant.conf`，随后打包进镜像。

### 2. 当前板级默认值

当前默认使用的板级配置为：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

已改成：

```sh
export RK_ENABLE_WIFI=y
export RK_ENABLE_WIFI_CHIP=AR9271
export LF_WIFI_SSID="byd"
export LF_WIFI_PSK="12345qwert"
```

另外，W 系列板型也同步写入了相同默认值：

- `BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Ultra_W-IPC.mk`
- `BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Pi_W-IPC.mk`
- `BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_86Panel_W-IPC.mk`

### 3. DHCP 自动获取

为了让开机联网不仅能启动 `wpa_supplicant`，还能自动获取 IP，本轮还补了：

- `project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-glibc-ultra/usr/bin/wifi_bt_init.sh`
- `sysdrv/tools/board/emmc/emmc_wifi_bt_init.sh`

在启动 `wpa_supplicant` 后自动执行：

```sh
udhcpc -i wlan0 -T 1 -A 0 -b -q &
```

## 运行时唯一配置约束

后续要求是“尽量保持配置文件唯一”。

本轮板端验证时，最终收敛到的唯一生效配置文件是：

- `/data/wpa_supplicant.conf`

板端验证中已确认：

- `rkwifi_server` / `wpa_supplicant` 实际读取的是 `/data/wpa_supplicant.conf`
- 该文件内容已成功切换为：

```sh
network={
	ssid="Rx3YYRpLkj"
	psk="12345qwert"
	key_mgmt=WPA-PSK
}
```

因此后续若继续完善自动联网逻辑，原则应为：

- 明确只保留一个运行时来源
- 避免 `/etc`、`/data`、`/userdata` 同类配置并存且相互覆盖

## AR9271 连接问题与修复

### 1. 初始问题

`AR9271` 网卡在当前板端环境下的表现是：

- 可以扫描
- 可以认证和关联
- 但始终无法完成 `WPA2` 建链

抓到的关键日志为：

```text
wlan0: WPA: Installing PTK to the driver
nl80211: set_key failed; err=-2 No such file or directory)
wlan0: WPA: Failed to set PTK to the driver
```

这说明问题点在：

- `4-way handshake` 最后阶段
- `PTK` 安装到驱动失败

### 2. 根因

`ath9k_htc` 在当前板端环境下使用硬件加密路径时，`PTK` 安装失败。

驱动源码支持模块参数：

- `nohwcrypt`

板端实测证明：

- 默认模式下连接失败
- 改为 `nohwcrypt=1` 后，连接立即成功

### 3. 修复方式

修改文件：

- `sysdrv/drv_ko/wifi/insmod_wifi.sh`

把 `AR9271` 分支从：

```sh
insmod ath9k_htc.ko
```

改为：

```sh
insmod ath9k_htc.ko nohwcrypt=1
```

### 4. 板端验证结果

在 `nohwcrypt=1` 下，板端已成功连接 `Rx3YYRpLkj`，并拿到：

- IP：`172.20.10.12/28`
- 网关：`172.20.10.1`

并且已实测：

```sh
ping 172.20.10.1
```

成功，丢包率为 `0%`。

## AP 扫描结论

本轮串口实测扫描到的典型 AP 包括：

- `Rx3YYRpLkj`
- `byd`
- `BUPT-portal`
- `ESP_EB66D0`

其中：

- `Rx3YYRpLkj` 一度为最强信号
- `byd` 也可见
- `BUPT-portal` 为开放网络

扫描工作已确认：

- `AR9271` 在当前环境下扫描功能正常
- 问题不在“扫不到 AP”
- 主要问题在 `WPA2` 建链时的硬件加密路径

## RKIPC 启动链

`rkipc` 的启动路径为：

1. 构建期生成 `/etc/init.d/S21appinit`
2. `S21appinit` 在 `start)` 分支执行：

```sh
sh /oem/usr/bin/RkLunch.sh
```

3. `RkLunch.sh` 会准备：

- `/userdata/rkipc.ini`
- IQ 文件目录 `/oem/usr/share/iqfiles`

4. 然后真正启动：

```sh
rkipc -a /oem/usr/share/iqfiles &
```

当前板端运行时看到的命令行也与此一致。

## RKIPC 配置来源

当前运行时配置文件路径是：

- `/userdata/rkipc.ini`

`RkLunch.sh` 会从：

- `/oem/usr/share/rkipc-*.ini`

挑选默认配置，再拷贝到：

- `/userdata/rkipc.ini`

因此后续如果要改默认视频参数、RTSP/RTMP 开关或编码方式，应优先从：

- `/oem/usr/share/` 下的出厂 ini
- 以及 `/userdata/rkipc.ini`

这两层关系来理解。

## RTSP 服务结论

### 1. 默认行为

当前这套 `rkipc` 不是“开机主动推流到远端服务器”，而是：

- 开机自动启动 `rkipc`
- 自动监听 `RTSP`
- 等待客户端拉流

### 2. 实测状态

板端已确认：

- `RTSP` 端口 `554` 在监听
- 当前 `rkipc.ini` 中 `enable_rtsp = 1`
- 当前 `enable_rtmp = 0`

因此当前主要是：

- 自动开启 RTSP 拉流服务
- 不是默认主动向远端 RTMP/RTSP 服务器推送

### 3. 可访问地址

在板端拿到 IP `172.20.10.12` 后，RTSP 地址可按下列方式访问：

- `rtsp://172.20.10.12/live/0`
- `rtsp://172.20.10.12/live/1`

推荐测试命令：

```sh
ffplay -rtsp_transport tcp rtsp://172.20.10.12/live/0
```

如果主码流不通，再试：

```sh
ffplay -rtsp_transport tcp rtsp://172.20.10.12/live/1
```

## 结论

本轮已经完成并验证的结论是：

- 镜像已支持默认写入 Wi-Fi SSID/PSK
- 运行时应尽量收敛到单一 Wi-Fi 配置来源
- `AR9271` 的根因不是“密码错”本身，而是硬件加密路径导致 `PTK` 安装失败
- `AR9271` 在 `nohwcrypt=1` 下可稳定完成 `WPA2-PSK` 连接
- 板端当前已成功获取无线 IP，并能 ping 通网关
- `rkipc` 开机自动启动，RTSP 服务默认可用
