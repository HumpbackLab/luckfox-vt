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

exec ./ipc_lite "$@"
