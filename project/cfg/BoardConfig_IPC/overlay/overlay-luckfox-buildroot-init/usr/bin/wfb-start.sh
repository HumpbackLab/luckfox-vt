#!/bin/sh

set -eu

MODE="${1:-}"
IFACE="${WFB_IFACE:-wlan0}"
CHANNEL="${WFB_CHANNEL:-149}"
WIDTH="${WFB_WIDTH:-HT20}"
KEY_DIR="${WFB_KEY_DIR:-/etc/wfb}"
KEY_PATH="${WFB_KEY:-}"
TX_PORT="${WFB_TX_PORT:-5600}"
RX_RADIO_PORT="${WFB_RX_RADIO_PORT:-0}"
RX_CLIENT_ADDR="${WFB_RX_CLIENT_ADDR:-127.0.0.1}"
RX_CLIENT_PORT="${WFB_RX_CLIENT_PORT:-5800}"
TUN_NAME="${WFB_TUN_NAME:-wfb0}"
TUN_ADDR="${WFB_TUN_ADDR:-10.5.0.1/24}"
PEER="${WFB_PEER:-127.0.0.1}"
TUN_PEER_PORT="${WFB_TUN_PEER_PORT:-5801}"
TUN_LISTEN_PORT="${WFB_TUN_LISTEN_PORT:-5800}"

usage() {
	echo "Usage: $0 {monitor|tx|rx|keygen}"
	echo "Env: WFB_IFACE WFB_CHANNEL WFB_WIDTH WFB_KEY_DIR WFB_KEY WFB_TX_PORT"
	echo "     WFB_RX_RADIO_PORT WFB_RX_CLIENT_ADDR WFB_RX_CLIENT_PORT"
	echo "     WFB_TUN_NAME WFB_TUN_ADDR WFB_PEER WFB_TUN_PEER_PORT WFB_TUN_LISTEN_PORT"
}

require_bin() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "Missing binary: $1" >&2
		exit 1
	}
}

prepare_monitor() {
	require_bin ip
	require_bin iw

	ip link set "$IFACE" down
	iw dev "$IFACE" set monitor otherbss
	ip link set "$IFACE" up
	iw dev "$IFACE" set channel "$CHANNEL" "$WIDTH"
}

ensure_keys() {
	mkdir -p "$KEY_DIR"
	if [ ! -f "$KEY_DIR/gs.key" ] || [ ! -f "$KEY_DIR/drone.key" ]; then
		(
			cd "$KEY_DIR"
			wfb_keygen
		)
	fi
}

case "$MODE" in
monitor)
		prepare_monitor
		iw dev
		;;
tx)
		require_bin wfb_tx
		prepare_monitor
		ensure_keys
		: "${KEY_PATH:=$KEY_DIR/drone.key}"
		exec wfb_tx -K "$KEY_PATH" -u "$TX_PORT" "$IFACE"
		;;
	rx)
		require_bin wfb_rx
		require_bin wfb_tun
		prepare_monitor
		ensure_keys
		: "${KEY_PATH:=$KEY_DIR/gs.key}"
		wfb_tun -t "$TUN_NAME" -a "$TUN_ADDR" -c "$PEER" -u "$TUN_PEER_PORT" -l "$TUN_LISTEN_PORT" &
		exec wfb_rx -K "$KEY_PATH" -c "$RX_CLIENT_ADDR" -u "$RX_CLIENT_PORT" -p "$RX_RADIO_PORT" "$IFACE"
		;;
	keygen)
		require_bin wfb_keygen
		mkdir -p "$KEY_DIR"
		cd "$KEY_DIR"
		exec wfb_keygen
		;;
	*)
		usage >&2
		exit 1
		;;
esac
