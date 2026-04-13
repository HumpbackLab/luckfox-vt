#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="/oem/usr/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

cd "$SCRIPT_DIR"
exec ./ipc_lite -c ./ipc_lite.ini "$@"

