# mis5001 驱动内循环工具

目标：修改 `sysdrv/source/kernel/drivers/media/i2c/mis5001.c` 后，不再走“整内核重编 + 整镜像替换”。

新增工具：

- [project/scripts/mis5001-dev.sh](/home/ncer/luckfox-vt/project/scripts/mis5001-dev.sh:1)

## 1. 首次准备

首次只需要把当前 SDK 的 kernel module 构建基座准备到 `/tmp`：

```sh
project/scripts/mis5001-dev.sh prepare --reset
```

这一步会：

- 使用当前仓库里的 SDK、toolchain、board config
- 在 `/tmp/luckfox-vt-objs_kernel` 创建可写的 kernel `O=` 输出目录
- 跑 `defconfig + modules_prepare + scripts`

之后反复改 `mis5001.c` 时，不需要重新准备。

## 2. 日常内循环

改完驱动后，直接跑：

```sh
project/scripts/mis5001-dev.sh build
project/scripts/mis5001-dev.sh deploy
project/scripts/mis5001-dev.sh reboot-reload
```

它会依次执行：

1. 单独编译 `drivers/media/i2c/mis5001.ko`
2. 上传到板子 `/oem/usr/ko/mis5001.ko.new`
3. 执行 `reboot-reload`
4. 停掉 `ipc_lite` / `rkipc`
5. 备份当前 `/oem/usr/ko/mis5001.ko` 为 `mis5001.ko.prev`
6. 用新模块替换当前模块文件并重启板子激活
7. 板子上线后打印近期 `dmesg` 中的 `mis5001 / sensor mode` 日志

## 3. 分步使用

只准备构建缓存：

```sh
project/scripts/mis5001-dev.sh prepare
```

只编译模块：

```sh
project/scripts/mis5001-dev.sh build
```

只上传：

```sh
project/scripts/mis5001-dev.sh deploy
```

替换模块文件并重启板子激活：

```sh
project/scripts/mis5001-dev.sh reboot-reload
```

查看板端状态：

```sh
project/scripts/mis5001-dev.sh board-state
```

查看相关日志：

```sh
project/scripts/mis5001-dev.sh logs
```

## 4. 默认参数

默认使用：

- board config: `project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk`
- objdir: `/tmp/luckfox-vt-objs_kernel`
- board: `root@192.168.3.23`
- remote ko dir: `/oem/usr/ko`

都可以通过参数覆盖，例如：

```sh
project/scripts/mis5001-dev.sh reboot-reload \
  --board-ip 192.168.3.23 \
  --objdir /tmp/luckfox-vt-objs_kernel
```

## 5. 当前限制

- 这套流程避免了“重编整个 kernel / 重刷整机镜像”，但不再尝试 `rmmod/insmod` 热更新
- 如果有摄像头业务占着 sensor，脚本会在重启前先尝试停掉 `ipc_lite` 和 `rkipc`
- 当前这块板子上，`mis5001` 常见 `use count 1`，因此只保留“替换模块文件 + 重启激活”这条通路
- 实际推荐内循环是：
  1. `project/scripts/mis5001-dev.sh build`
  2. `project/scripts/mis5001-dev.sh deploy`
  3. `project/scripts/mis5001-dev.sh reboot-reload`

不再保留：

- `reload`
- `cycle`
- `revert`
- `--keep-running`
- `--reboot-fallback`
