# ipc_lite media notes

## Low-latency UDP/RTP receive

Use `gst_udp_rtp_lowlat.sh` on the receiver PC to receive the H.264 RTP stream.
The script listens on UDP port `5600` by default and lets GStreamer detect the
resolution and frame rate from the H.264 stream.

```sh
project/app/ipc_lite/gst_udp_rtp_lowlat.sh
project/app/ipc_lite/gst_udp_rtp_lowlat.sh 5602
```

The receiver pipeline is intentionally small:

```text
udpsrc
! rtpjitterbuffer latency=0 drop-on-latency=true
! rtph264depay
! avdec_h264
! queue max-size-buffers=1 leaky=downstream
! videoconvert
! video sink sync=false async=false
```

`avdec_h264` is a software decoder. The script chooses `ximagesink` when
available, then falls back to `glimagesink` or `autovideosink`.

Set `GST_VIDEO_SINK` to override the display sink:

```sh
GST_VIDEO_SINK=glimagesink project/app/ipc_lite/gst_udp_rtp_lowlat.sh 5600
```

Stop a local receiver with:

```sh
pkill -x gst-launch-1.0
```

## Probe examples

Run `v4l2_mpi_venc_probe` from the board workspace:

```sh
cd /root/ipc_lite
export LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib
```

720p at 90 fps:

```sh
./v4l2_mpi_venc_probe -c ipc_lite.720p90.fast.udp.ini -n 0 --drain
```

720p at 60 fps:

```sh
./v4l2_mpi_venc_probe -c ipc_lite.720p60.fast.udp.ini -n 0 --drain
```

1080p at 60 fps:

```sh
./v4l2_mpi_venc_probe -c ipc_lite.1080p60.fast.udp.ini -n 0 --drain
```

Use `-n 0` for an unlimited run. Use a positive frame count for a bounded test.
Keep file output disabled for long UDP/RTP latency tests.
