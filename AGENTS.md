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
- Do not assume `1280x720` is stable on this board.
- Existing low-level issues such as `csi size err`, `PIC_SIZE_ERROR`, and frame loss may come from the sensor/CSI path, not only from `ipc_lite`.

## Practical Rules

- Before using serial, make sure no stale `miniterm`, `monitor.sh`, or old `ssh/scp` sessions are still holding resources.
- Before starting a new media test, ensure no stale `ipc_lite` process is still running on the board.
- Prefer short, reproducible test runs first, then move to longer RTSP/ffplay validation.
