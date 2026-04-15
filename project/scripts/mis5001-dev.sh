#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SDK_ROOT=$(CDPATH= cd -- "$PROJECT_DIR/.." && pwd)

DEFAULT_BOARD_CONFIG="$SDK_ROOT/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk"
DEFAULT_OBJDIR="/tmp/luckfox-vt-objs_kernel"
DEFAULT_BOARD_IP="192.168.3.23"
DEFAULT_BOARD_USER="root"
DEFAULT_BOARD_PASSWORD="luckfox"
DEFAULT_REMOTE_KO_DIR="/oem/usr/ko"
DEFAULT_REMOTE_KO_NAME="mis5001.ko"

BOARD_CONFIG="${MIS5001_BOARD_CONFIG:-}"
OBJDIR="${MIS5001_OBJDIR:-$DEFAULT_OBJDIR}"
BOARD_IP="${MIS5001_BOARD_IP:-$DEFAULT_BOARD_IP}"
BOARD_USER="${MIS5001_BOARD_USER:-$DEFAULT_BOARD_USER}"
BOARD_PASSWORD="${MIS5001_BOARD_PASSWORD:-$DEFAULT_BOARD_PASSWORD}"
REMOTE_KO_DIR="${MIS5001_REMOTE_KO_DIR:-$DEFAULT_REMOTE_KO_DIR}"
REMOTE_KO_NAME="${MIS5001_REMOTE_KO_NAME:-$DEFAULT_REMOTE_KO_NAME}"

ARCH_VALUE=""
CROSS_PREFIX=""
TOOLCHAIN_BIN=""
KERNEL_DEFCONFIG=""
KERNEL_DEFCONFIG_FRAGMENT=""

usage() {
  cat <<'EOF'
Usage:
  project/scripts/mis5001-dev.sh <command> [options]

Commands:
  prepare        Bootstrap a writable kernel objdir in /tmp for module builds
  build          Build only drivers/media/i2c/mis5001.ko
  deploy         Upload the built .ko to the board as mis5001.ko.new
  reboot-reload  Swap in mis5001.ko.new, reboot the board, verify it came back
  logs           Show recent board dmesg lines related to mis5001/camera mode
  board-state    Show current board module paths and load status

Options:
  --board-config <path>   Override board config file
  --objdir <path>         Override writable kernel objdir
  --board-ip <ip>         Override board IP
  --board-user <user>     Override board username
  --board-password <pw>   Override board password
  --remote-ko-dir <dir>   Override board module directory
  --reset                 Recreate objdir during prepare

Examples:
  project/scripts/mis5001-dev.sh prepare --reset
  project/scripts/mis5001-dev.sh build
  project/scripts/mis5001-dev.sh deploy
  project/scripts/mis5001-dev.sh reboot-reload
EOF
}

log() {
  printf '[mis5001-dev] %s\n' "$*"
}

die() {
  printf '[mis5001-dev] error: %s\n' "$*" >&2
  exit 1
}

resolve_board_config() {
  if [ -n "$BOARD_CONFIG" ]; then
    [ -f "$BOARD_CONFIG" ] || die "board config not found: $BOARD_CONFIG"
    return
  fi

  if [ -f "$SDK_ROOT/.BoardConfig.mk" ]; then
    BOARD_CONFIG="$SDK_ROOT/.BoardConfig.mk"
    return
  fi

  BOARD_CONFIG="$DEFAULT_BOARD_CONFIG"
  [ -f "$BOARD_CONFIG" ] || die "default board config not found: $BOARD_CONFIG"
}

load_board_config() {
  resolve_board_config
  # shellcheck disable=SC1090
  source "$BOARD_CONFIG"

  ARCH_VALUE="${RK_ARCH:-arm}"
  CROSS_PREFIX="${RK_TOOLCHAIN_CROSS:-arm-rockchip830-linux-uclibcgnueabihf}"
  KERNEL_DEFCONFIG="${RK_KERNEL_DEFCONFIG:-rv1106_defconfig}"
  KERNEL_DEFCONFIG_FRAGMENT="${RK_KERNEL_DEFCONFIG_FRAGMENT:-}"
  TOOLCHAIN_BIN="$SDK_ROOT/tools/linux/toolchain/$CROSS_PREFIX/bin"

  [ -x "$TOOLCHAIN_BIN/${CROSS_PREFIX}-gcc" ] || \
    die "toolchain not found: $TOOLCHAIN_BIN/${CROSS_PREFIX}-gcc"

  export PATH="$TOOLCHAIN_BIN:$PATH"
}

kernel_make() {
  make -C "$SDK_ROOT/sysdrv/source/kernel" \
    O="$OBJDIR" \
    ARCH="$ARCH_VALUE" \
    CROSS_COMPILE="${CROSS_PREFIX}-" \
    "$@"
}

prepare_objdir() {
  local reset="${1:-0}"

  load_board_config

  if [ "$reset" = "1" ]; then
    log "resetting objdir: $OBJDIR"
    rm -rf "$OBJDIR"
  fi

  mkdir -p "$OBJDIR"

  if [ ! -f "$OBJDIR/.config" ]; then
    log "generating kernel config in $OBJDIR"
    if [ -n "$KERNEL_DEFCONFIG_FRAGMENT" ]; then
      kernel_make "$KERNEL_DEFCONFIG" $KERNEL_DEFCONFIG_FRAGMENT
    else
      kernel_make "$KERNEL_DEFCONFIG"
    fi
  fi

  log "preparing module build environment in $OBJDIR"
  kernel_make modules_prepare scripts
}

build_module() {
  load_board_config
  [ -f "$OBJDIR/.config" ] || die "objdir not prepared: $OBJDIR (run prepare first)"

  log "building mis5001.ko"
  kernel_make drivers/media/i2c/mis5001.ko
  [ -f "$OBJDIR/drivers/media/i2c/mis5001.ko" ] || \
    die "build finished without mis5001.ko"

  log "built $OBJDIR/drivers/media/i2c/mis5001.ko"
}

ssh_board_raw() {
  sshpass -p "$BOARD_PASSWORD" ssh \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/tmp/luckfox_known_hosts \
    -o ConnectTimeout=5 \
    "$BOARD_USER@$BOARD_IP" "$@"
}

ssh_board_retry() {
  local attempt

  for attempt in 1 2 3; do
    if ssh_board_raw "$@"; then
      return 0
    fi
    if [ "$attempt" -lt 3 ]; then
      log "ssh retry $attempt failed, retrying"
      sleep 1
    fi
  done

  return 1
}

scp_board() {
  local attempt

  for attempt in 1 2 3; do
    if sshpass -p "$BOARD_PASSWORD" scp \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/tmp/luckfox_known_hosts \
      -o ConnectTimeout=5 \
      "$@"; then
      return 0
    fi
    if [ "$attempt" -lt 3 ]; then
      log "scp retry $attempt failed, retrying"
      sleep 1
    fi
  done

  return 1
}

wait_for_board_ssh() {
  local timeout_sec="${1:-90}"
  local start_ts

  start_ts=$(date +%s)
  while :; do
    if ssh_board_raw "true" >/dev/null 2>&1; then
      return 0
    fi
    if [ $(( $(date +%s) - start_ts )) -ge "$timeout_sec" ]; then
      return 1
    fi
    sleep 2
  done
}

deploy_module() {
  [ -f "$OBJDIR/drivers/media/i2c/mis5001.ko" ] || die "missing built module in $OBJDIR"

  log "uploading module to $BOARD_USER@$BOARD_IP:$REMOTE_KO_DIR/${REMOTE_KO_NAME}.new"
  ssh_board_retry "mkdir -p '$REMOTE_KO_DIR'"
  scp_board "$OBJDIR/drivers/media/i2c/mis5001.ko" \
    "$BOARD_USER@$BOARD_IP:$REMOTE_KO_DIR/${REMOTE_KO_NAME}.new"
  ssh_board_retry "sha256sum '$REMOTE_KO_DIR/${REMOTE_KO_NAME}.new'"
}

reboot_reload_module() {
  local remote_ko="$REMOTE_KO_DIR/$REMOTE_KO_NAME"
  local remote_new="${remote_ko}.new"
  local remote_prev="${remote_ko}.prev"

  log "installing module on board and rebooting for activation"
  ssh_board_raw "
set -eu
test -f '$remote_new' || { echo 'missing uploaded module: $remote_new' >&2; exit 1; }
killall ipc_lite 2>/dev/null || true
killall rkipc 2>/dev/null || true
sleep 1
if [ -f '$remote_ko' ]; then
  cp -af '$remote_ko' '$remote_prev'
fi
cp -af '$remote_new' '$remote_ko'
sync
reboot
" >/dev/null 2>&1 || true

  log "waiting for board to come back after reboot"
  sleep 3
  wait_for_board_ssh 120 || die "board did not come back after reboot"
  log "board is reachable again"
  show_board_state
  show_logs
}

show_logs() {
  ssh_board_retry "dmesg | grep -i 'mis5001\\|sensor mode\\|csi size err\\|PIC_SIZE_ERROR' | tail -n 80"
}

show_board_state() {
  ssh_board_retry "
set -eu
uname -a
echo ---
ls -l '$REMOTE_KO_DIR' | grep 'mis5001\\.ko' || true
echo ---
cat /proc/modules | grep mis5001 || true
"
}

command_name="${1:-}"
[ -n "$command_name" ] || {
  usage
  exit 1
}
shift

reset_objdir=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --board-config)
      BOARD_CONFIG="$2"
      shift 2
      ;;
    --objdir)
      OBJDIR="$2"
      shift 2
      ;;
    --board-ip)
      BOARD_IP="$2"
      shift 2
      ;;
    --board-user)
      BOARD_USER="$2"
      shift 2
      ;;
    --board-password)
      BOARD_PASSWORD="$2"
      shift 2
      ;;
    --remote-ko-dir)
      REMOTE_KO_DIR="$2"
      shift 2
      ;;
    --reset)
      reset_objdir=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

case "$command_name" in
  prepare)
    prepare_objdir "$reset_objdir"
    ;;
  build)
    build_module
    ;;
  deploy)
    deploy_module
    ;;
  reboot-reload)
    reboot_reload_module
    ;;
  logs)
    show_logs
    ;;
  board-state)
    show_board_state
    ;;
  *)
    usage
    die "unknown command: $command_name"
    ;;
esac
