#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="/oem/usr/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

cd "$SCRIPT_DIR"

if [ "$#" -eq 0 ]; then
  set -- -c "${IPC_LITE_CONFIG:-./ipc_lite.ini}"
elif [ "$#" -eq 1 ] && [ "${1#-}" = "$1" ]; then
  set -- -c "$1"
fi

config_path=""
if [ "$#" -ge 2 ] && [ "$1" = "-c" ]; then
  config_path="$2"
fi

maybe_force_sensor_mode() {
  local cfg="$1"
  local width=""
  local height=""

  [ -n "$cfg" ] || return 0
  [ -f "$cfg" ] || return 0
  command -v media-ctl >/dev/null 2>&1 || return 0

  width=$(
    awk '
      /^\[video\]/ { in_video=1; next }
      /^\[/ { in_video=0 }
      in_video && $1 == "width" && $2 == "=" { print $3; exit }
    ' "$cfg"
  )
  height=$(
    awk '
      /^\[video\]/ { in_video=1; next }
      /^\[/ { in_video=0 }
      in_video && $1 == "height" && $2 == "=" { print $3; exit }
    ' "$cfg"
  )

  [ -n "$width" ] || return 0
  [ -n "$height" ] || return 0

  media-ctl -d /dev/media0 \
    --set-v4l2 "'m00_b_mis5001 4-0031':0[fmt:SGRBG10_1X10/${width}x${height}]" \
    >/dev/null 2>&1 || true
}

maybe_force_sensor_mode "$config_path"

exec ./ipc_lite "$@"
