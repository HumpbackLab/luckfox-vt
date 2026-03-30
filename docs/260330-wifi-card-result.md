# AR9271 USB Wi-Fi 适配记录

## 背景

目标是在当前 SDK 中适配 USB Wi-Fi 网卡 Atheros/Qualcomm AR9271。

本文件记录的是本轮“适配 wifi card”任务的最终结果，不只包含最后两处最小修复，也包含此前为验证和补齐适配链路做过的内核、DTS、Buildroot、板级配置和分析文档修改。

已知板上环境：

- Buildroot 2023.02.6
- kernel 5.10.160
- USB 设备可正常枚举，`lsusb` 可见 `0cf3:9271`
- 板上不提供标准 `/lib/modules`，不能依赖 `modprobe`
- 当前驱动加载方式为 `/oem/usr/ko/*.ko` + `insmod_ko.sh` / `insmod_wifi.sh`

本次实际使用的板级配置为：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

## 初始现象

板上 USB 已识别到 AR9271，但启动后没有 `wlan0`。

排查后确认：

- `/oem/usr/ko` 中已有内核无线模块
- 固件也已存在
- 当前问题不是 USB 枚举失败
- 当前问题是自动加载链没有覆盖 AR9271，对应模块没有在启动时被串起来

## 仓库内检查结果

### 1. 启动调用链是成立的

仓库中现有调用链如下：

- `rc.local` / `RkLunch.sh`
- `/oem/usr/ko/insmod_ko.sh`
- `/oem/usr/ko/insmod_wifi.sh`

其中：

- `sysdrv/drv_ko/insmod_ko.sh` 末尾会后台执行 `insmod_wifi.sh`
- overlay 中的 `rc.local` 也会执行 `insmod_ko.sh`
- `RkLunch.sh` 中同样有执行 `insmod_ko.sh` 的路径

结论：

- 启动链本身不需要重构
- 问题点集中在 `insmod_wifi.sh` 缺少 AR9271 分支

### 2. `insmod_wifi.sh` 缺少 AR9271 自动加载逻辑

原脚本已覆盖 AIC、Broadcom、Realtek、SSV、ATBM 等分支，但没有针对 `0cf3:9271` 的 USB 检测和加载逻辑。

这能解释现象：

- USB 已枚举
- 但 `cfg80211/mac80211/ath9k_htc` 这条依赖链没有被执行
- 因此不会出现 `wlan0`

## 板上手工验证过程

在旧镜像板上直接手工 `insmod` 做了验证。

### 第一次尝试

按常规依赖顺序加载时失败，`mac80211.ko` 报错：

- `Unknown symbol arc4_setkey`
- `Unknown symbol arc4_crypt`

随后 `ath9k_htc.ko` 又继续报大量 `ieee80211_*` 符号缺失。

### 根因确认

问题不是 `ath9k_htc` 本身，而是前置依赖少了 `libarc4.ko`。

补上以后，以下顺序可正常拉起 AR9271：

```sh
insmod cfg80211.ko
insmod libarc4.ko
insmod mac80211.ko
insmod ath.ko
insmod ath9k_hw.ko
insmod ath9k_common.ko
insmod ath9k_htc.ko
```

板上随后出现关键日志：

- `ath9k_htc: Firmware ath9k_htc/htc_9271-1.4.0.fw requested`
- `Transferred FW: ath9k_htc/htc_9271-1.4.0.fw`
- `ath9k_htc: HTC initialized with 33 credits`
- `ieee80211 phy0: Atheros AR9271 Rev:1`

同时接口状态正常：

- `ifconfig -a` 可见 `wlan0`
- `ip link show` 可见 `wlan0`
- `iw dev` 可见 `phy#0` 下的 `wlan0`

结论：

- 最小可用依赖顺序必须包含 `libarc4.ko`

## 本次仓库修改

本轮任务实际落到仓库中的改动分为 6 类。

### 1. 新增 AR9271 自动识别与加载分支

修改文件：

- `sysdrv/drv_ko/wifi/insmod_wifi.sh`

新增逻辑：

```sh
#ath9k_htc ar9271
cat /sys/bus/usb/devices/*/uevent | grep -i "cf3\/9271"
if [ $? -eq 0 ]; then
	insmod cfg80211.ko
	insmod libarc4.ko
	insmod mac80211.ko
	insmod ath.ko
	insmod ath9k_hw.ko
	insmod ath9k_common.ko
	insmod ath9k_htc.ko
fi
```

作用：

- 延续现有脚本风格
- 继续使用 `cat /sys/bus/usb/devices/*/uevent | grep ...` 检测方式
- 在检测到 `0cf3:9271` 时按已验证顺序加载驱动

### 2. 给当前实际板级配置开启 Wi-Fi 打包

修改文件：

- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`

新增配置：

```sh
export RK_ENABLE_WIFI=y
export RK_ENABLE_WIFI_CHIP=AR9271
```

这一步分两层作用：

- `RK_ENABLE_WIFI=y`
  使 Wi-Fi 构建路径生效，确保 `insmod_wifi.sh` 会被复制到最终镜像
- `RK_ENABLE_WIFI_CHIP=AR9271`
  避免 `sysdrv/drv_ko/wifi/Makefile` 在 `RK_ENABLE_WIFI_CHIP` 为空时进入“全量构建所有 Wi-Fi 驱动”的兜底分支

### 3. W 系列板型补齐 USB Wi-Fi 相关 kernel fragment

修改文件：

- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Ultra_W-IPC.mk`
- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Pi_W-IPC.mk`
- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_86Panel_W-IPC.mk`

变更点：

- 在 `RK_KERNEL_DEFCONFIG_FRAGMENT` 中加入 `rv1106-usbwifi.config`

作用：

- 让 W 系列板型在 kernel 配置层显式合入 USB Wi-Fi 相关模块配置
- 避免只有蓝牙 fragment、生效范围不足的问题

### 4. 内核与设备树层补齐 AR9271 所需基础条件

修改文件：

- `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1103g-luckfox-pico-mini.dts`

变更点：

- `luckfox_rv1106_linux_defconfig` 中补入：
  - `CONFIG_CFG80211=m`
  - `CONFIG_MAC80211=m`
  - `CONFIG_RFKILL=y`
  - `CONFIG_WLAN_VENDOR_ATH=y`
  - `CONFIG_ATH9K_HTC=m`
- `rv1103g-luckfox-pico-mini.dts` 中把 USB `dr_mode` 从 `peripheral` 改为 `host`

作用：

- 在 kernel 配置层补齐 AR9271 驱动栈
- 在设备树层确保 USB 口工作于可枚举外置网卡的 host 模式

### 5. Buildroot rootfs 补齐固件与用户态工具

修改文件：

- `sysdrv/tools/board/buildroot/luckfox_pico_defconfig`
- `sysdrv/tools/board/buildroot/luckfox_pico_w_defconfig`

新增内容包括：

- `BR2_PACKAGE_IW=y`
- `BR2_PACKAGE_LINUX_FIRMWARE=y`
- `BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_9271=y`
- `BR2_PACKAGE_WPA_SUPPLICANT=y`
- `BR2_PACKAGE_WPA_SUPPLICANT_NL80211=y`
- `BR2_PACKAGE_WPA_SUPPLICANT_CLI=y`
- `BR2_PACKAGE_WPA_SUPPLICANT_PASSPHRASE=y`

作用：

- 把 AR9271 固件带进 rootfs
- 补齐后续用户态联网所需的基础工具

### 6. 过程与结论文档

新增文档：

- `docs/260329-wifi-card.md`
- `docs/260330-wifi-card-result.md`

作用：

- `260329-wifi-card.md` 保留前期分析、假设和补丁方向
- `260330-wifi-card-result.md` 记录最终落地结果和上板验证结论

## 中间遇到的构建问题

在只加 `RK_ENABLE_WIFI=y` 时，构建触发了额外的 Wi-Fi 驱动全量编译，最终在 `bcmdhd` 上失败：

- `rockchip_wifi_mac_addr`
- `rockchip_wifi_get_oob_irq_flag`
- `rockchip_wifi_set_carddetect`
- `rockchip_wifi_get_oob_irq`
- `rockchip_wifi_power`
- `sdio_reset_comm`

根因是：

- `sysdrv/drv_ko/wifi/Makefile` 在 `RK_ENABLE_WIFI_CHIP` 为空时会编译所有 USB/SDIO 驱动
- 其中 `bcmdhd` 依赖 Rockchip 私有 Wi-Fi glue 符号
- 当前板型并不需要它

因此最终改法不是修改 `wifi/Makefile`，而是在当前板级配置中补一个非空的 `RK_ENABLE_WIFI_CHIP=AR9271`，以最小范围绕开该兜底逻辑。

## 上板最终结果

串口确认结果如下：

- `/oem/usr/ko/insmod_wifi.sh` 已存在
- 脚本中已包含 AR9271 自动加载分支
- 开机后这些模块已自动加载：
  - `cfg80211`
  - `libarc4`
  - `mac80211`
  - `ath`
  - `ath9k_hw`
  - `ath9k_common`
  - `ath9k_htc`

内核日志：

- `ath9k_htc: Firmware ath9k_htc/htc_9271-1.4.0.fw requested`
- `Transferred FW: ath9k_htc/htc_9271-1.4.0.fw`
- `ath9k_htc: HTC initialized with 33 credits`
- `ieee80211 phy0: Atheros AR9271 Rev:1`

接口状态：

- `ifconfig -a` 有 `wlan0`
- `ip link show` 有 `wlan0`
- `iw dev` 可见 `phy#0` 下 `wlan0, type managed`

结论：

- “USB 已枚举但没有 wlan0”的问题已经解决
- 当前修复已完成“识别网卡并自动加载驱动，启动后产生 wlan 接口”的目标

## 当前未继续处理的项

### 1. 用户态联网流程未启动

当前板上虽然驱动和 `wlan0` 已经正常起来，但：

- `rkwifi_server` 没有在跑
- `wpa_supplicant` 没有在跑

这不影响“驱动已成功适配”的结论，但如果后续要真正联网，还需要继续确认：

- 谁负责拉起 Wi-Fi 用户态管理流程
- 使用哪套脚本或服务来配置 `wpa_supplicant`

### 2. `regulatory.db` 告警

日志中仍有：

- `cfg80211: failed to load regulatory.db`

这不会阻止 `wlan0` 出现，目前不是本轮主问题。

## 本次任务纳入提交的文件

- `docs/260329-wifi-card.md`
- `docs/260330-wifi-card-result.md`
- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_86Panel_W-IPC.mk`
- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Pi_W-IPC.mk`
- `project/cfg/BoardConfig_IPC/BoardConfig-EMMC-Buildroot-RV1106_Luckfox_Pico_Ultra_W-IPC.mk`
- `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`
- `sysdrv/drv_ko/wifi/insmod_wifi.sh`
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1103g-luckfox-pico-mini.dts`
- `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`
- `sysdrv/tools/board/buildroot/luckfox_pico_defconfig`
- `sysdrv/tools/board/buildroot/luckfox_pico_w_defconfig`
