# Wi-Fi / WFB-NG 实施规划

本文档记录当前仓库后续的实现规划。

当前决策：

- Wi-Fi / WFB-NG 路线优先选择 `RTL8812AU`
- `AR9271` 保留为备选验证路径，不再作为主线推进
- WFB-NG 优先落地最小数据面，不先做 full Python 控制面

相关参考：

- `docs/260330-wfb-ng.md`
- `docs/260330-wifi-card-result.md`
- `https://github.com/svpcom/wfb-ng/tree/wfb-ng-25.01.2`
- `https://github.com/svpcom/rtl8812au`

## 1. 目标

目标拆成两层：

### 1.1 第一目标

让当前 SDK 能稳定支持 `RTL8812AU` USB Wi-Fi 网卡，至少达到：

- USB 枚举正常
- 驱动自动加载
- 出现 `wlan0`
- 可切换到 `monitor` 模式
- 可用于后续 WFB-NG 注入测试

### 1.2 第二目标

在 `RTL8812AU` 驱动跑通后，把 WFB-NG 最小数据面接入当前 SDK，至少包括：

- `wfb_tx`
- `wfb_rx`
- `wfb_keygen`
- 可选 `wfb_tun`

不作为第一阶段目标的内容：

- `wfb-server`
- `wfb-cli`
- systemd 服务
- 完整 cluster 模式
- 用户态 STA 联网功能完善

## 2. 为什么优先 RTL8812AU

相比 AR9271，`RTL8812AU` 更适合作为 WFB-NG 主线方案，原因如下：

### 2.1 上游明确支持

`wfb-ng-25.01.2` 官方 README 对 `RTL8812AU` 的定位是：

- `stable`
- 但要求使用作者维护的 patched driver

而 AR9271 只是：

- `not supported, but may works`

因此从方案风险看：

- `RTL8812AU` 是上游主路径
- `AR9271` 是非主路径

### 2.2 WFB-NG 默认围绕 8812AU/8812EU 驱动生态设计

上游脚本和 README 里明确依赖以下驱动名：

- `rtl88xxau_wfb`
- `rtl88x2eu`

例如：

- `scripts/wfb-nics`
- `scripts/install_gs.sh`
- README 中 `ethtool -i wlan0` 的检查项

这说明：

- 上游测试矩阵主要覆盖 8812AU/8812EU
- 周边脚本、经验和调参都更成熟

### 2.3 本地仓库并非没有 8812AU 基础

当前仓库没有现成的 `8812au.ko` 成品输出，也没有 `RTL8812AU` 自动识别分支，但并不是完全空白。

现有证据：

- [sysdrv/drv_ko/wifi/rtl8188ftv/Makefile](/work/luckfox-pico/sysdrv/drv_ko/wifi/rtl8188ftv/Makefile#L32) 中内置：
  - `CONFIG_RTL8812A = n`
  - `MODULE_NAME = 8812au`
- 同一驱动树内已存在 monitor / radiotap 相关实现：
  - `rtw_monitor_xmit_entry`
  - radiotap header 处理
  - monitor netdev 类型处理

这说明当前 SDK 的 Wi-Fi 构建框架可以容纳 `RTL8812AU`。

## 3. 当前仓库现状

### 3.1 当前板级配置仍指向 AR9271

当前实际板级配置：

- [project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk](/work/luckfox-pico/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk#L1)

其中当前设置为：

```sh
export RK_ENABLE_WIFI=y
export RK_ENABLE_WIFI_CHIP=AR9271
```

这意味着当前主线产物仍然偏向 AR9271。

### 3.2 当前 Wi-Fi 自动加载脚本没有 RTL8812AU 分支

当前脚本：

- [sysdrv/drv_ko/wifi/insmod_wifi.sh](/work/luckfox-pico/sysdrv/drv_ko/wifi/insmod_wifi.sh#L1)

已覆盖：

- AIC8800
- AP6XXX
- RTL8723BS
- RTL8189FS
- RTL8188FU
- SSV
- AR9271
- ATBM

但没有：

- `RTL8812AU`

因此即便后续把模块编出来，当前启动链也不会自动加载它。

### 3.3 当前 Wi-Fi 顶层构建入口没有 RTL8812AU 选项

当前顶层构建入口：

- [sysdrv/drv_ko/wifi/Makefile](/work/luckfox-pico/sysdrv/drv_ko/wifi/Makefile#L43)

USB 分支当前仅显式支持：

- `RTL8188FTV`
- `SSV6X5X`
- `SSV6115`
- `AIC8800DW_USB`
- `AIC8800MC`

没有：

- `RTL8812AU`

所以当前 `RK_ENABLE_WIFI_CHIP=RTL8812AU` 还不会触发有效构建。

### 3.4 当前产物里没有 8812AU 模块

当前输出目录：

- `output/out/oem/usr/ko`

未见：

- `8812au.ko`
- `88XXau_wfb.ko`
- `rtl88xxau_wfb.ko`

说明 8812AU 当前未真正进入产物。

## 4. 主实施路线

主线采用：

- 优先集成 `svpcom/rtl8812au`
- 不优先复用当前 `rtl8188ftv` 作为最终量产驱动

原因：

### 4.1 优先选择 `svpcom/rtl8812au`

优势：

- 与 `wfb-ng` 官方驱动配套
- 上游 README 和安装脚本直接围绕它写
- 驱动名、monitor/injection 能力与 WFB 预期更一致
- 后续遇到问题时更容易对照上游经验

### 4.2 本地 `rtl8188ftv` 作为备选

`rtl8188ftv` 仍有价值，但定位为：

- 备选技术参考
- 用于理解 Realtek 驱动如何在当前 SDK 里接入
- 在外部驱动接入遇阻时，可做临时试验路径

不建议第一阶段就押注它的原因：

- 当前默认目标芯片不是 8812A
- 当前平台宏与 RV1106/RV1103 不完全匹配
- 与 `wfb-ng` 官方驱动名和预期能力不完全一致
- 后续问题排查成本更高

## 5. 分阶段计划

## 5.1 阶段 A：接入 RTL8812AU 驱动

阶段目标：

- 让 `RTL8812AU` 模块进入 `output/out/oem/usr/ko`
- 开机自动加载成功
- 出现 `wlan0`

### A1. 引入驱动源码

计划：

- 在 `sysdrv/drv_ko/wifi/` 下新增 `rtl8812au/`
- 来源优先使用 `svpcom/rtl8812au`

要求：

- 保持独立目录
- 不覆盖现有 `rtl8188ftv`
- 尽量少改上游代码，先用最小 patch 适配当前 SDK

### A2. 接入顶层 Wi-Fi 构建

修改目标：

- [sysdrv/drv_ko/wifi/Makefile](/work/luckfox-pico/sysdrv/drv_ko/wifi/Makefile#L43)

计划新增：

- `RK_ENABLE_WIFI_CHIP=RTL8812AU` 的 USB 构建分支
- 对应 `make -C rtl8812au/`

同时补 clean 分支。

### A3. 板级配置切到 RTL8812AU

修改目标：

- [project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk](/work/luckfox-pico/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk#L1)

计划改为：

```sh
export RK_ENABLE_WIFI=y
export RK_ENABLE_WIFI_CHIP=RTL8812AU
```

目的：

- 让 Wi-Fi 构建路径明确只生成 8812AU 所需驱动
- 避免再次落入“全量编所有 Wi-Fi 驱动”的分支

### A4. 自动加载脚本增加 8812AU 识别

修改目标：

- [sysdrv/drv_ko/wifi/insmod_wifi.sh](/work/luckfox-pico/sysdrv/drv_ko/wifi/insmod_wifi.sh#L1)

计划新增：

- 基于 USB VID/PID 的 8812AU 识别
- 对应模块加载顺序

这里要注意：

- 不同 8812AU 网卡 VID/PID 可能不同
- 初版可先覆盖常见 `0bda:*` 设备
- 更稳妥做法是根据驱动最终 `MODULE_ALIAS` 或实际实物 `lsusb` 结果补充

### A5. 与现有 `rkwifi_server` 的关系

当前 `insmod_wifi.sh` 末尾会在看到 `wlan0` 后自动启动：

- `rkwifi_server`

对 8812AU 路线，阶段 A 的处理原则是：

- 先不依赖 `rkwifi_server`
- 只要求驱动加载后 `wlan0` 存在

必要时可以临时调整为：

- 检测到 `RTL8812AU` 时跳过 `rkwifi_server`
- 或至少不让它覆盖 monitor 模式设置

因为 WFB 最终不是普通 STA 联网模式。

## 5.2 阶段 B：验证 RTL8812AU 具备 WFB 所需无线能力

阶段目标：

- 确认 `RTL8812AU` 在当前板上能用于 WFB

必须验证的点：

### B1. 驱动识别正确

验证项：

- `ifconfig -a`
- `ip link show`
- `iw dev`
- `ethtool -i wlan0`

理想结果：

- `wlan0` 存在
- 驱动名接近上游预期，例如 `rtl88xxau_wfb`

### B2. monitor 模式可切换

验证命令示例：

```sh
ip link set wlan0 down
iw dev wlan0 set monitor otherbss
ip link set wlan0 up
iw dev wlan0 set channel 149 HT20
iw dev
```

期望：

- 切换成功
- 接口未消失
- 无明显 kernel oops

### B3. radiotap / injection 能力存在

验证方向：

- monitor 模式下可接收 radiotap 帧
- 发包路径不拒绝注入

这一步如果驱动名和行为完全对齐 `wfb-ng` 官方方案，成功率会明显高于 AR9271。

## 5.3 阶段 C：接入 WFB-NG 最小数据面

阶段目标：

- 把 `wfb_tx / wfb_rx / wfb_keygen` 编入镜像

### C1. 补 Buildroot 依赖

修改目标：

- [sysdrv/tools/board/buildroot/luckfox_pico_defconfig](/work/luckfox-pico/sysdrv/tools/board/buildroot/luckfox_pico_defconfig#L1)
- 视需要同步到 `luckfox_pico_w_defconfig`

新增建议：

```text
BR2_PACKAGE_LIBPCAP=y
BR2_PACKAGE_LIBSODIUM=y
BR2_PACKAGE_LIBEVENT=y
BR2_PACKAGE_ETHTOOL=y
BR2_PACKAGE_TCPDUMP=y
```

说明：

- `libpcap` 是 `wfb_rx` 必需
- `libsodium` 是 `wfb_tx/wfb_rx/wfb_keygen` 必需
- `libevent` 只对 `wfb_tun` 必需，但建议一次补上
- `ethtool` 和 `tcpdump` 便于板上验证

### C2. 新增 WFB-NG package

建议新增一个本地 Buildroot package，例如：

- `sysdrv/source/buildroot/buildroot-2023.02.6/package/wfb-ng-min/`

首阶段只做：

- 下载或引用本地源码
- 交叉编译 `all_bin`
- 安装：
  - `wfb_tx`
  - `wfb_rx`
  - `wfb_keygen`
  - 可选 `wfb_tun`

不做：

- Python package 安装
- systemd 文件安装
- `wfb-server`
- `wfb-cli`

### C3. 新增本地启动脚本

建议新增：

- `/usr/bin/wfb-start-tx.sh`
- `/usr/bin/wfb-start-rx.sh`

或一个统一脚本：

- `/usr/bin/wfb-start.sh`

职责应尽量简单：

1. 关闭 `wlan0`
2. 切 monitor mode
3. 设置信道
4. 拉起 `wfb_tx` 或 `wfb_rx`

这样便于隔离问题：

- 驱动问题
- monitor 模式问题
- WFB 二进制问题

## 5.4 阶段 D：视验证结果决定是否接 full 控制面

只有在阶段 C 已经稳定后，再考虑：

- `python-twisted`
- `python-msgpack`
- `python-pyyaml`
- `python-pyroute2`
- `wfb-server`
- `wfb-cli`
- `/etc/wifibroadcast.cfg`

当前不建议提前投入这一层。

## 6. 文件级实施清单

预计修改范围如下。

### 6.1 板级配置

- [project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk](/work/luckfox-pico/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk#L1)

计划：

- `RK_ENABLE_WIFI_CHIP` 从 `AR9271` 改为 `RTL8812AU`

### 6.2 Wi-Fi 顶层构建

- [sysdrv/drv_ko/wifi/Makefile](/work/luckfox-pico/sysdrv/drv_ko/wifi/Makefile#L43)

计划：

- 增加 `RTL8812AU` 分支
- 新增 `rtl8812au/` 的 build / clean 入口

### 6.3 Wi-Fi 自动加载

- [sysdrv/drv_ko/wifi/insmod_wifi.sh](/work/luckfox-pico/sysdrv/drv_ko/wifi/insmod_wifi.sh#L1)

计划：

- 增加 `RTL8812AU` USB 识别
- 增加对应模块加载逻辑
- 视情况避免 `rkwifi_server` 干扰 WFB 使用场景

### 6.4 新驱动目录

- `sysdrv/drv_ko/wifi/rtl8812au/`

计划：

- 接入 `svpcom/rtl8812au`
- 增加本 SDK 所需最小适配补丁

### 6.5 Buildroot defconfig

- [sysdrv/tools/board/buildroot/luckfox_pico_defconfig](/work/luckfox-pico/sysdrv/tools/board/buildroot/luckfox_pico_defconfig#L1)
- [sysdrv/tools/board/buildroot/luckfox_pico_w_defconfig](/work/luckfox-pico/sysdrv/tools/board/buildroot/luckfox_pico_w_defconfig#L1)

计划：

- 增加 `libpcap`
- 增加 `libsodium`
- 增加 `libevent`
- 增加 `ethtool`
- 增加 `tcpdump`

### 6.6 Buildroot package

- `sysdrv/source/buildroot/buildroot-2023.02.6/package/wfb-ng-min/`

计划：

- 最小二进制 package

### 6.7 启动脚本

建议新增：

- `project/cfg/BoardConfig_IPC/overlay/.../usr/bin/wfb-start.sh`
- 或 `project/cfg/BoardConfig_IPC/overlay/.../etc/init.d/S??wfb`

## 7. 验证计划

验证顺序必须从下往上，不要跳步。

### 7.1 驱动构建验证

验证点：

- `output/out/oem/usr/ko/` 中出现 8812AU 模块
- 构建不再依赖无关芯片分支

### 7.2 开机自动加载验证

验证点：

- 插入 RTL8812AU 网卡后自动加载模块
- `dmesg` 无明显 crash
- `wlan0` 出现

### 7.3 monitor 模式验证

验证点：

- `iw dev wlan0 set monitor otherbss` 成功
- 切换后接口仍可 `up`
- 可设置 channel

### 7.4 WFB 最小链路验证

验证点：

- 两端能运行 `wfb_keygen`
- 一端 `wfb_tx`
- 一端 `wfb_rx`
- 能观察到稳定接收

### 7.5 才考虑 Python full 版

如果前面都没过，不进入这一阶段。

## 8. 风险清单

### 8.1 驱动平台适配风险

风险：

- `svpcom/rtl8812au` 需要为当前 kernel 5.10.160 和 RV1106 SDK 环境做 patch

缓解：

- 保持驱动独立目录
- 首先做最小编译通过

### 8.2 USB 供电 / 带宽风险

风险：

- 8812AU 一般比 AR9271 更吃供电
- 单 USB 口供电不足时会导致不稳定

缓解：

- 上板时优先确认 USB 供电能力
- 必要时使用外供电 HUB

### 8.3 `rkwifi_server` 干扰风险

风险：

- 当前系统默认把无线视为普通联网设备
- monitor/WFB 模式可能被重新配置

缓解：

- WFB 验证阶段禁止自动联网逻辑介入

### 8.4 Full 控制面过早投入风险

风险：

- Python 依赖一旦提前接入，问题面会迅速扩大

缓解：

- 先只做最小数据面

## 9. 明确不做的事项

当前阶段不做：

- 把 AR9271 继续打造成主线 WFB 方案
- 同时并行做 AR9271 和 RTL8812AU 的完整适配
- 一开始就移植 `wfb-server` / `wfb-cli`
- 一开始就做 cluster 模式
- 一开始就做隧道功能

## 10. 推荐执行顺序

推荐按以下顺序推进：

1. 把板级 `RK_ENABLE_WIFI_CHIP` 切到 `RTL8812AU`
2. 引入 `svpcom/rtl8812au` 驱动目录
3. 修改 `sysdrv/drv_ko/wifi/Makefile` 接入 8812AU 构建
4. 修改 `insmod_wifi.sh` 加自动识别和加载
5. 出第一版镜像，验证 `wlan0`
6. 验证 `monitor otherbss`
7. Buildroot 补 `libpcap/libsodium/libevent`
8. 接入 `wfb-ng-min` package
9. 做最小 `wfb_tx/wfb_rx` 联调
10. 再决定是否进入 full 控制面

## 11. 当前推荐结论

当前主线建议非常明确：

- **优先 RTL8812AU**
- **优先官方 WFB 驱动路线**
- **优先最小数据面**
- **延后 full Python 控制面**

这条路线的成功率、可维护性和与上游一致性，都优于继续主推 AR9271。
