#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="/oem/usr/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

cd "$SCRIPT_DIR"

if [ "$#" -eq 0 ]; then
  set -- -c ./ipc_lite.ini
fi

exec ./ipc_lite "$@"
