# RTL8812AU monitor/managed 可切换说明

## 背景

当前 SDK 中，`RTL8812AU` 既要支持后续 `monitor/WFB` 使用场景，也需要在调试阶段临时切到普通联网模式。

此前 `sysdrv/drv_ko/wifi/insmod_wifi.sh` 的行为是：

- 检测到 `rtl88xxau_wfb.ko` 或 `88XXau_wfb.ko` 后，默认不启动任何 Wi-Fi 用户态
- 直接打印 `RTL8812AU detected. Keep wlan0 unmanaged for monitor/WFB use.`

这对 `WFB` 是对的，但不适合“当前先联网、后续再回 monitor”的调试流程。

本次改动目标不是删除 `monitor/WFB` 路径，而是把它改成“默认保留，必要时可显式覆盖”。

## 本次改动

修改文件：

- `sysdrv/drv_ko/wifi/insmod_wifi.sh`

新增逻辑包括：

- 增加 `WIFI_MODE_FILE=/userdata/wifi_mode`
- 增加 `get_wifi_mode()`
- 增加 `should_keep_wlan0_unmanaged()`
- 增加 `start_wifi_userspace()`
- 保留 `RTL8812AU_SKIP_RKWIFI_SERVER`
- 仅当实际加载的是 `*_wfb.ko` 时，才把 `RTL8812AU_SKIP_RKWIFI_SERVER=1`

行为变化如下：

1. 默认行为不变

- 如果加载的是 `rtl88xxau_wfb.ko` / `88XXau_wfb.ko`
- 且没有设置任何覆盖模式
- 仍然保持 `wlan0` 不进入 managed 用户态

2. 可通过运行时开关覆盖

- 若环境变量 `RK_WIFI_MODE=managed`
- 或文件 `/userdata/wifi_mode` 内容为 `managed`
- 则即使当前使用的是 `*_wfb.ko`，也允许继续拉起普通联网用户态

3. 用户态启动策略更明确

- 优先启动 `rkwifi_server`
- 若板端没有 `rkwifi_server`，则回退到 `wpa_supplicant`
- 若两者都没有，则只保留驱动，不继续拉起用户态

## 关键实现点

### 1. monitor/WFB 默认保留

`load_rtl8812au_module()` 里不再对所有 `RTL8812AU` 模块一刀切跳过用户态，而是只对 `*_wfb.ko` 设置：

```sh
case "$module" in
*_wfb.ko)
	RTL8812AU_SKIP_RKWIFI_SERVER=1
	;;
esac
```

这样普通 `8812au.ko` 不会被误判成 `WFB` 模式。

### 2. managed 模式覆盖入口

脚本优先读取：

- 环境变量 `RK_WIFI_MODE`
- 文件 `/userdata/wifi_mode`

当值为以下任意一种时，允许进入普通联网模式：

- `managed`
- `sta`
- `client`

当值为空或以下任意一种时，继续保留 monitor/WFB：

- 空
- `monitor`
- `wfb`

未知值也会保守处理为“不启动用户态”。

### 3. 用户态拉起顺序

`start_wifi_userspace()` 的处理顺序是：

1. 确认 `wlan0` 存在
2. 若当前应保留 unmanaged，则直接返回
3. 若存在 `rkwifi_server`，执行 `rkwifi_server start &`
4. 否则若存在 `wpa_supplicant` 和 `/etc/wpa_supplicant.conf`，则：
   - `ifconfig wlan0 up`
   - 清理旧的 `wpa_supplicant` / `dhcpcd` / `udhcpc`
   - 重建 `/var/run/wpa_supplicant`
   - 启动 `wpa_supplicant`
   - 优先用 `dhcpcd`，否则回退到 `udhcpc`

## 板端使用方式

### 临时切到普通联网模式

```sh
echo managed >/userdata/wifi_mode
sh /oem/usr/ko/insmod_wifi.sh
```

如果板端只有 `wpa_supplicant`，没有 `rkwifi_server`，则会自动回退到 `wpa_supplicant`。

### 恢复 monitor/WFB 模式

```sh
rm -f /userdata/wifi_mode
killall wpa_supplicant dhcpcd udhcpc 2>/dev/null
ifconfig wlan0 down
sh /oem/usr/ko/insmod_wifi.sh
```

这样下次重新执行 Wi-Fi 初始化脚本时，会恢复成 `RTL8812AU` 的默认 monitor/WFB 行为。

### 只对当前一次生效

如果不想写 `/userdata/wifi_mode`，也可以只在当前进程环境中覆盖：

```sh
RK_WIFI_MODE=managed sh /oem/usr/ko/insmod_wifi.sh
```

命令结束后不会留下持久化状态。

## 与当前板端实测状态的关系

本轮串口检查到的板端状态是：

- `wlan0` 已存在
- `88XXau_wfb`、`cfg80211`、`mac80211` 已加载
- 当前未运行 `rkwifi_server`
- 当前未运行 `wpa_supplicant`
- 当前 rootfs 中有 `wpa_supplicant`、`wpa_cli`
- 当前 rootfs 中没有 `rkwifi_server`

因此这次回退逻辑是必要的：

- 对当前板子，切到 `managed` 后更可能走 `wpa_supplicant`
- 对后续若重新把 `rkwifi_server` 打进 rootfs 的镜像，则仍可优先走 Rockchip 自带用户态

## 结论

本次改动的核心不是把 `RTL8812AU` 从 `monitor/WFB` 改成永久 `managed`，而是：

- 保留 `monitor/WFB` 为默认行为
- 增加一个显式、可恢复、运行时可控的 `managed` 覆盖入口
- 让同一张卡可以在调试联网与后续 monitor 使用之间切换，而不需要删除已有 `WFB` 路径
