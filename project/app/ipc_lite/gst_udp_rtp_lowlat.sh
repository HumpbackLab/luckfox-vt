#!/bin/sh

set -eu

PORT="${1:-5600}"

usage() {
  cat <<EOF
Usage:
  $0 [port]

Examples:
  $0
  $0 5600
  $0 5602
EOF
}

case "${PORT}" in
  -h|--help)
    usage
    exit 0
    ;;
  *[!0-9]*)
    echo "invalid port: ${PORT}" >&2
    usage >&2
    exit 1
    ;;
esac

command -v gst-launch-1.0 >/dev/null 2>&1 || {
  echo "gst-launch-1.0 not found in PATH" >&2
  exit 1
}

gst-inspect-1.0 udpsrc >/dev/null 2>&1 || {
  echo "missing GStreamer element: udpsrc" >&2
  exit 1
}
gst-inspect-1.0 rtpjitterbuffer >/dev/null 2>&1 || {
  echo "missing GStreamer element: rtpjitterbuffer" >&2
  exit 1
}
gst-inspect-1.0 rtph264depay >/dev/null 2>&1 || {
  echo "missing GStreamer element: rtph264depay" >&2
  exit 1
}
gst-inspect-1.0 avdec_h264 >/dev/null 2>&1 || {
  echo "missing GStreamer element: avdec_h264" >&2
  exit 1
}

SINK="${GST_VIDEO_SINK:-}"
SINK_OPTS="sync=false async=false"
if [ -z "${SINK}" ]; then
  if gst-inspect-1.0 ximagesink >/dev/null 2>&1; then
    SINK="ximagesink"
  elif gst-inspect-1.0 glimagesink >/dev/null 2>&1; then
    SINK="glimagesink"
  elif gst-inspect-1.0 autovideosink >/dev/null 2>&1; then
    SINK="autovideosink"
    SINK_OPTS="sync=false"
  else
    echo "missing GStreamer video sink: ximagesink/glimagesink/autovideosink" >&2
    exit 1
  fi
fi

echo "Receiving RTP/H264 on UDP port ${PORT} with ${SINK}" >&2

exec gst-launch-1.0 -v \
  udpsrc port="${PORT}" caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
  ! rtpjitterbuffer latency=0 drop-on-latency=true do-lost=true \
  ! rtph264depay \
  ! avdec_h264 skip-frame=0 output-corrupt=false \
  ! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream \
  ! videoconvert \
  ! "${SINK}" ${SINK_OPTS}
