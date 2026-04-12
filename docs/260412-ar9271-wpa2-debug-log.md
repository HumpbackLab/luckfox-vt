# AR9271 WPA2 连接调试记录

## 背景

目标板卡使用当前仓库实际选中的板级配置：

- `BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk`
- `RK_ENABLE_WIFI_CHIP=AR9271`

板端现象稳定为：

- 能扫描到目标 AP
- 能完成 `authenticated` / `associated`
- 随后立即出现本地主动 `deauthenticating ... by local choice`
- `wpa_state` 回到 `SCANNING`

## 初始现象

内核日志反复出现：

```text
wlan0: authenticated
wlan0: associate with <bssid> (try 1/3)
wlan0: associated
wlan0: deauthenticating from <bssid> by local choice (Reason: 1=UNSPECIFIED)
```

板端确认项：

- `ath9k_htc` 模块参数 `/sys/module/ath9k_htc/parameters/nohwcrypt = 1`
- 当前镜像已带 `insmod ath9k_htc.ko nohwcrypt=1`
- 板端运行用户态时只有一组：
  - `rkwifi_server`
  - `wpa_supplicant`
  - `udhcpc`

因此可先排除：

- 密码明显错误
- AP 侧立即踢人
- `nohwcrypt=1` 未生效
- 用户态重复启动导致的互相杀进程

## 关键排查过程

### 1. 排除 `rkwifi_server` 干扰

在板端停止：

```sh
killall rkwifi_server udhcpc wpa_supplicant
ifconfig wlan0 down
ifconfig wlan0 up
rm -rf /var/run/wpa_supplicant
mkdir -p /var/run/wpa_supplicant
wpa_supplicant -dd -i wlan0 -c /data/wpa_supplicant.conf
```

结果：问题仍然稳定复现。

由此确认问题不依赖 `rkwifi_server`，而是在更底层的

- `wpa_supplicant`
- `nl80211`
- `cfg80211/mac80211`
- `ath9k_htc`

这条链路里。

### 2. 直接抓到失败点

`wpa_supplicant -dd` 关键日志如下：

```text
wlan0: WPA: Installing PTK to the driver
nl80211: set_key failed; err=-2 No such file or directory)
wlan0: WPA: Failed to set PTK to the driver
wlan0: WPA: 4-Way Handshake failed - pre-shared key may be incorrect
```

这说明失败发生在：

- `4-Way Handshake`
- `PTK` 安装阶段
- `NL80211_CMD_NEW_KEY`

而不是 DHCP。

### 3. 确认失败发生在 `NL80211_CMD_NEW_KEY`

在内核中加临时诊断后，板端得到：

```text
nl80211_new_key: iftype=2 idx=0 pairwise=1 ... allowed_ret=0
nl80211_new_key: ops_add_key=ieee80211_add_key [mac80211]
rdev_add_key: netdev=wlan0 idx=0 pairwise=1 ... ret=-2
nl80211_new_key: add_key ret=-2 ...
```

这说明：

- `nl80211_key_allowed()` 已通过
- `cfg80211` 确实调用到了 `mac80211` 的 `ieee80211_add_key`
- `-ENOENT` 是 `add_key` 这条路径返回上来的

### 4. 从软件加密依赖继续收口

`AR9271` 走 `nohwcrypt=1` 时，需要由 `mac80211` 软件完成 `CCMP`。

板端当时的 `lsmod` 只有：

- `ath9k_htc`
- `ath9k_common`
- `ath9k_hw`
- `ath`
- `mac80211`
- `libarc4`
- `cfg80211`

缺少软件 `CCMP` 所需 crypto 模块。

而 `mac80211` 的 `CCMP` 密钥分配路径会调用：

```c
crypto_alloc_aead("ccm(aes)", ...)
```

继续核对后发现：

- `ccm.ko` 存在，但内部依赖 `ctr(...)`
- 板端 `/proc/crypto` 中没有出现：
  - `ccm(aes)`
  - `ctr(aes)`
  - `ecb(aes)`

这说明软件 `CCMP` 所需算法实例没有注册成功。

### 5. 手工补模块验证

在板端手工加载：

```sh
insmod /oem/usr/ko/libaes.ko
insmod /oem/usr/ko/aes_generic.ko
insmod /oem/usr/ko/ccm.ko
insmod /oem/usr/ko/ctr.ko
```

然后重新运行：

```sh
killall rkwifi_server udhcpc wpa_supplicant
ifconfig wlan0 down
ifconfig wlan0 up
rm -rf /var/run/wpa_supplicant
mkdir -p /var/run/wpa_supplicant
wpa_supplicant -B -dd -f /tmp/wpa.log -i wlan0 -c /data/wpa_supplicant.conf
```

此时内核日志变为：

```text
drv_set_key ... ops_set_key=ath9k_htc_set_key [ath9k_htc]
ath: phy0: set_key: nohwcrypt=1 ...
drv_set_key ... ret=-28
key_enable_hw_accel ... ret=-28
add_key exit ... err=0
rdev_add_key ... ret=0
```

同时：

```text
wpa_state=COMPLETED
ip_address=172.20.10.12
```

并且路由也正确生成：

```text
default via 172.20.10.1 dev wlan0
172.20.10.0/28 dev wlan0 scope link src 172.20.10.12
```

## 根因

根因不是：

- AP 配置错误
- 密码错误
- `rkwifi_server` 干扰
- `nohwcrypt=1` 未生效

根因是：

- `AR9271` 已切到软件加密路径
- 但自动加载脚本没有把软件 `CCMP` 所需 crypto 模块加载完整
- 尤其缺少 `ctr.ko`
- 导致 `ccm(aes)` 无法正常提供给 `mac80211`
- 最终 `NL80211_CMD_NEW_KEY` 返回 `-ENOENT`

## 最终修复

修改文件：

- `sysdrv/drv_ko/wifi/insmod_wifi.sh`

在 `AR9271` 分支中，除了原本的：

- `cfg80211.ko`
- `libarc4.ko`
- `mac80211.ko`
- `ath.ko`
- `ath9k_hw.ko`
- `ath9k_common.ko`
- `ath9k_htc.ko nohwcrypt=1`

额外补齐：

- `ctr.ko`
- `ccm.ko`
- `libaes.ko`
- `aes_generic.ko`

## 结论

本轮最终结论是：

1. `AR9271` 的 `WPA2-PSK` 连接必须继续保留 `nohwcrypt=1`
2. 仅有 `nohwcrypt=1` 还不够
3. 还必须在模块加载时补齐软件 `CCMP` 依赖，尤其是 `ctr.ko`

否则现象就会稳定表现为：

- 能关联
- 但 `Installing PTK` 时 `set_key failed; err=-2`
- 随后本地 `deauth`

