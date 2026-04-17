# AGENTS

## Scope

This file captures the shared board-debugging conventions for this repository.
Use it as the default operational baseline unless the user explicitly overrides it.

## Board Baseline

- Board family: Luckfox Pico
- SoC: RV1106
- OS: Buildroot-based Linux
- Default board app workspace: `/root/ipc_lite`
- IQ files directory: `/oem/usr/share/iqfiles`
- Common sensor under test: `mis5001`

## Credentials

- Username: `root`
- Password: `luckfox`
- Board IP: use the currently assigned Luckfox board IP address.
- Receiver PC IP: use the current PC/client IP only for UDP/RTP, RTSP client, or
  other host-side receiving endpoints.

## Access Priority

Prefer network access first.

1. Use `ssh` for interactive board work.
2. Use `scp` for file transfer.
3. Use the serial console only as a fallback when networking is unavailable or broken.

Reason:

- `ssh/scp` is much faster than serial upload.
- It is easier to keep a foreground process running and inspect logs over `ssh`.
- Serial is still useful for early boot logs, recovery, and network failure cases.

## Serial Fallback

- Serial helper script: `/work/luckfox-pico/monitor.sh`
- Current serial helper behavior:

```sh
./monitor.sh
```

- This opens the board console on `/dev/ttyUSB0` at `115200`.

Use serial when:

- the board has not joined Wi-Fi yet
- `ssh`/`scp` is unreachable
- boot-time kernel logs are needed
- network configuration itself is under investigation

## Deployment Conventions

### `mis5001` Kernel Module

When building or deploying the `mis5001` kernel driver, use the repository helper
script instead of hand-written `scp` / `ssh` module replacement commands:

```sh
MIS5001_BOARD_IP=<board-ip> project/scripts/mis5001-dev.sh build
MIS5001_BOARD_IP=<board-ip> project/scripts/mis5001-dev.sh deploy
MIS5001_BOARD_IP=<board-ip> project/scripts/mis5001-dev.sh reboot-reload
```

Useful inspection commands:

```sh
MIS5001_BOARD_IP=<board-ip> project/scripts/mis5001-dev.sh board-state
MIS5001_BOARD_IP=<board-ip> project/scripts/mis5001-dev.sh logs
```

Notes:

- `<board-ip>` means the Luckfox board IP address, not the receiving PC /
  RTP/UDP client IP.
- The script reads `.BoardConfig.mk` when present; in this repository that selects
  `RK_KERNEL_DEFCONFIG=luckfox_rv1106_linux_defconfig`.
- The script builds against `/tmp/luckfox-vt-objs_kernel` by default.
- `deploy` uploads the new module as `/oem/usr/ko/mis5001.ko.new`.
- `reboot-reload` swaps `.new` into `/oem/usr/ko/mis5001.ko`, preserves the
  previous module as `.prev`, reboots the board, waits for it to return, then
  prints board state and recent camera logs.
- Do not manually overwrite `/oem/usr/ko/mis5001.ko` unless the user explicitly
  requests bypassing the helper script.

### `ipc_lite`

When deploying `ipc_lite`, prefer:

```sh
scp project/app/ipc_lite/out/ipc_lite/{ipc_lite,ipc_lite.ini,run.sh} root@<board-ip>:/root/ipc_lite/
ssh root@<board-ip> 'cd /root/ipc_lite && chmod +x ipc_lite run.sh && ./run.sh'
```

Only fall back to serial upload if network transfer is not possible.

## Runtime Conventions

- Run from: `/root/ipc_lite`
- Default launch:

```sh
cd /root/ipc_lite
./run.sh
```

- Keep the process in the foreground during debugging unless background execution is explicitly needed.

## Media Debugging Notes

- Treat `704x576 + selfpath + H.265` as the current conservative baseline.
- Treat `1280x720 + selfpath + H.264 + 25fps` as the current exploratory low-latency baseline when `RTSP` validation is needed.
- Do not assume `1280x720 + 30fps` is stable on this board.
- Current `720p selfpath` is not a native `MIS5001` sensor mode:
  - the sensor still comes up as `2592x1944`
  - `720p` is currently produced downstream by ISP/VI scaling or cropping
- If the goal is higher frame rate such as `60fps` or `120fps`, do not treat it as an `ipc_lite`-only tuning problem.
  - The critical path is likely `MIS5001` native sensor mode support plus matching `IQ` / ISP tuning.
- Existing low-level issues such as `csi size err`, `PIC_SIZE_ERROR`, and frame loss may come from the sensor/CSI path, not only from `ipc_lite`.

## Practical Rules

- Before using serial, make sure no stale `miniterm`, `monitor.sh`, or old `ssh/scp` sessions are still holding resources.
- Before starting a new media test, ensure no stale `ipc_lite` process is still running on the board.
- When testing `720p` or higher, prefer `vi_channel = 1` (`rkisp_selfpath`) unless there is a specific reason to revisit `mainpath`.
- Keep `file = off` during RTSP and performance validation unless a short local capture is explicitly needed.
  - `/tmp` is `tmpfs`, and long file dumps can push the board into `OOM`.
- Prefer short, reproducible test runs first, then move to longer RTSP/ffplay validation.
- If a kernel replacement leaves the board unable to boot normally, it is acceptable
  to write a small host-side recovery script that opens the serial console and sends
  `Ctrl-C` to U-Boot so the boot environment can be inspected or repaired.
