#include "udp_rtp_sink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define IPC_LITE_RTP_HEADER_LEN 12
#define IPC_LITE_RTP_SSRC 0x22334455U

typedef struct {
  int fd;
  struct sockaddr_in remote_addr;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint8_t *pending_data;
  size_t pending_len;
  size_t pending_capacity;
  uint64_t pending_pts;
  bool pending_key_frame;
  bool has_pending;
  bool stop;
  bool thread_started;
  uint16_t seq;
  uint32_t timestamp_base;
  uint64_t pts_base;
  int mtu;
  int payload_type;
  unsigned int drop_count;
  unsigned int queue_drop_count;
} IPC_LITE_UDP_RTP_SINK_CONTEXT;

static bool udp_rtp_sender_should_preempt(IPC_LITE_UDP_RTP_SINK_CONTEXT *context) {
  bool preempt = false;

  pthread_mutex_lock(&context->lock);
  preempt = context->stop || context->has_pending;
  pthread_mutex_unlock(&context->lock);

  return preempt;
}

static bool find_start_code(const uint8_t *data, size_t len, size_t *offset,
                            size_t *prefix_len) {
  size_t i = 0;

  for (i = 0; i + 3 < len; ++i) {
    if (data[i] == 0x00 && data[i + 1] == 0x00) {
      if (data[i + 2] == 0x01) {
        *offset = i;
        *prefix_len = 3;
        return true;
      }
      if (i + 4 < len && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        *offset = i;
        *prefix_len = 4;
        return true;
      }
    }
  }

  return false;
}

static uint32_t packet_pts_to_rtp_ts(IPC_LITE_UDP_RTP_SINK_CONTEXT *context,
                                     uint64_t pts) {
  uint64_t delta_pts = 0;

  if (context->pts_base == 0) {
    context->pts_base = pts;
  }
  delta_pts = pts - context->pts_base;
  return context->timestamp_base + (uint32_t)((delta_pts * 9ULL) / 100ULL);
}

static int udp_send_rtp_packet(IPC_LITE_UDP_RTP_SINK_CONTEXT *context,
                               const uint8_t *payload, size_t payload_len,
                               uint32_t timestamp, bool marker) {
  uint8_t packet[1600];
  ssize_t sent = 0;

  if (payload_len + IPC_LITE_RTP_HEADER_LEN > sizeof(packet)) {
    return -1;
  }

  packet[0] = 0x80;
  packet[1] = (uint8_t)(context->payload_type & 0x7f);
  if (marker) {
    packet[1] |= 0x80;
  }
  packet[2] = (uint8_t)(context->seq >> 8);
  packet[3] = (uint8_t)(context->seq & 0xff);
  packet[4] = (uint8_t)(timestamp >> 24);
  packet[5] = (uint8_t)(timestamp >> 16);
  packet[6] = (uint8_t)(timestamp >> 8);
  packet[7] = (uint8_t)(timestamp & 0xff);
  packet[8] = (uint8_t)(IPC_LITE_RTP_SSRC >> 24);
  packet[9] = (uint8_t)(IPC_LITE_RTP_SSRC >> 16);
  packet[10] = (uint8_t)(IPC_LITE_RTP_SSRC >> 8);
  packet[11] = (uint8_t)(IPC_LITE_RTP_SSRC & 0xff);
  memcpy(packet + IPC_LITE_RTP_HEADER_LEN, payload, payload_len);

  sent = sendto(context->fd, packet, payload_len + IPC_LITE_RTP_HEADER_LEN, 0,
                (const struct sockaddr *)&context->remote_addr,
                sizeof(context->remote_addr));
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
      context->drop_count++;
    }
    return -1;
  }

  context->seq++;
  return 0;
}

static int udp_send_h264_nalu(IPC_LITE_UDP_RTP_SINK_CONTEXT *context,
                              const uint8_t *nalu, size_t nalu_len,
                              uint32_t timestamp, bool marker,
                              bool allow_preempt) {
  size_t max_payload = (size_t)(context->mtu - IPC_LITE_RTP_HEADER_LEN);
  size_t chunk = 0;
  size_t offset = 1;
  uint8_t fu_indicator = 0;
  uint8_t fu_header = 0;
  uint8_t payload[1600];

  if (nalu_len == 0 || max_payload < 2) {
    return -1;
  }

  if (nalu_len <= max_payload) {
    return udp_send_rtp_packet(context, nalu, nalu_len, timestamp, marker);
  }

  fu_indicator = (uint8_t)((nalu[0] & 0xe0) | 28U);
  while (offset < nalu_len) {
    bool start = (offset == 1);
    bool end = false;
    size_t remaining = nalu_len - offset;

    if (allow_preempt && udp_rtp_sender_should_preempt(context)) {
      return -1;
    }

    chunk = max_payload - 2;
    if (chunk > remaining) {
      chunk = remaining;
    }
    end = (offset + chunk) >= nalu_len;

    fu_header = (uint8_t)(nalu[0] & 0x1f);
    if (start) {
      fu_header |= 0x80;
    }
    if (end) {
      fu_header |= 0x40;
    }

    payload[0] = fu_indicator;
    payload[1] = fu_header;
    memcpy(payload + 2, nalu + offset, chunk);
    if (udp_send_rtp_packet(context, payload, chunk + 2, timestamp,
                            marker && end) != 0) {
      return -1;
    }
    offset += chunk;
  }

  return 0;
}

static int udp_rtp_sink_send_packet(IPC_LITE_UDP_RTP_SINK_CONTEXT *context,
                                    const IPC_LITE_STREAM_PACKET *packet) {
  size_t offset = 0;
  size_t prefix_len = 0;
  size_t nal_start = 0;
  size_t next_offset = 0;
  size_t next_prefix = 0;
  uint32_t timestamp = 0;

  if (!context || !packet || packet->len == 0) {
    return 0;
  }

  if (!find_start_code(packet->data, packet->len, &offset, &prefix_len)) {
    context->drop_count++;
    if (context->drop_count <= 5U) {
      IPC_LITE_LOGW("udp_rtp_sink",
                    "packet missing Annex-B start code, drop=%u",
                    context->drop_count);
    }
    return -1;
  }

  timestamp = packet_pts_to_rtp_ts(context, packet->pts);
  while (offset < packet->len) {
    bool marker = false;
    size_t nal_len = 0;
    const uint8_t *nalu = NULL;

    nal_start = offset + prefix_len;
    if (nal_start >= packet->len) {
      break;
    }

    if (find_start_code(packet->data + nal_start, packet->len - nal_start,
                        &next_offset, &next_prefix)) {
      nal_len = next_offset;
      offset = nal_start + next_offset;
      prefix_len = next_prefix;
    } else {
      nal_len = packet->len - nal_start;
      offset = packet->len;
      marker = true;
    }

    if (nal_len == 0) {
      continue;
    }

    if (!packet->key_frame && udp_rtp_sender_should_preempt(context)) {
      return -1;
    }

    nalu = packet->data + nal_start;
    if (udp_send_h264_nalu(context, nalu, nal_len, timestamp, marker,
                           !packet->key_frame) != 0) {
      return -1;
    }
  }

  return 0;
}

static void *udp_rtp_sender_thread(void *arg) {
  IPC_LITE_UDP_RTP_SINK_CONTEXT *context =
      (IPC_LITE_UDP_RTP_SINK_CONTEXT *)arg;
  uint8_t *local_data = NULL;
  size_t local_capacity = 0;

  while (true) {
    IPC_LITE_STREAM_PACKET packet;

    memset(&packet, 0, sizeof(packet));

    pthread_mutex_lock(&context->lock);
    while (!context->stop && !context->has_pending) {
      pthread_cond_wait(&context->cond, &context->lock);
    }
    if (context->stop && !context->has_pending) {
      pthread_mutex_unlock(&context->lock);
      break;
    }

    if (local_capacity < context->pending_len) {
      uint8_t *new_data = realloc(local_data, context->pending_len);
      if (!new_data) {
        context->has_pending = false;
        pthread_mutex_unlock(&context->lock);
        continue;
      }
      local_data = new_data;
      local_capacity = context->pending_len;
    }

    memcpy(local_data, context->pending_data, context->pending_len);
    packet.data = local_data;
    packet.len = context->pending_len;
    packet.pts = context->pending_pts;
    packet.key_frame = context->pending_key_frame;
    context->has_pending = false;
    pthread_mutex_unlock(&context->lock);

    udp_rtp_sink_send_packet(context, &packet);
  }

  free(local_data);
  return NULL;
}

static int udp_rtp_sink_open(IPC_LITE_STREAM_SINK *sink,
                             const IPC_LITE_CONFIG *config,
                             IPC_LITE_CODEC codec) {
  IPC_LITE_UDP_RTP_SINK_CONTEXT *context = NULL;
  struct timeval send_timeout;

  if (!config->udp_rtp.enable) {
    sink->enabled = false;
    return 0;
  }

  if (codec != IPC_LITE_CODEC_H264) {
    IPC_LITE_LOGE("udp_rtp_sink",
                  "udp_rtp sink currently supports H264 only");
    return -1;
  }

  context = calloc(1, sizeof(*context));
  if (!context) {
    return -1;
  }
  context->fd = -1;

  if (pthread_mutex_init(&context->lock, NULL) != 0) {
    free(context);
    return -1;
  }
  if (pthread_cond_init(&context->cond, NULL) != 0) {
    pthread_mutex_destroy(&context->lock);
    free(context);
    return -1;
  }

  context->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (context->fd < 0) {
    pthread_cond_destroy(&context->cond);
    pthread_mutex_destroy(&context->lock);
    free(context);
    return -1;
  }

  memset(&send_timeout, 0, sizeof(send_timeout));
  send_timeout.tv_usec = 2000;
  setsockopt(context->fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
             sizeof(send_timeout));

  memset(&context->remote_addr, 0, sizeof(context->remote_addr));
  context->remote_addr.sin_family = AF_INET;
  context->remote_addr.sin_port = htons((uint16_t)config->udp_rtp.port);
  if (inet_aton(config->udp_rtp.host, &context->remote_addr.sin_addr) == 0) {
    close(context->fd);
    pthread_cond_destroy(&context->cond);
    pthread_mutex_destroy(&context->lock);
    free(context);
    return -1;
  }

  context->seq = 1;
  context->timestamp_base = (uint32_t)time(NULL) * 90000U;
  context->mtu = config->udp_rtp.mtu;
  context->payload_type = config->udp_rtp.payload_type;
  context->pending_capacity = (size_t)config->video.venc_buffer_size;
  if (context->pending_capacity > 0) {
    context->pending_data = malloc(context->pending_capacity);
    if (!context->pending_data) {
      close(context->fd);
      pthread_cond_destroy(&context->cond);
      pthread_mutex_destroy(&context->lock);
      free(context);
      return -1;
    }
  }

  if (pthread_create(&context->thread, NULL, udp_rtp_sender_thread, context) !=
      0) {
    free(context->pending_data);
    close(context->fd);
    pthread_cond_destroy(&context->cond);
    pthread_mutex_destroy(&context->lock);
    free(context);
    return -1;
  }
  context->thread_started = true;

  sink->ctx = context;
  sink->enabled = true;
  IPC_LITE_LOGI("udp_rtp_sink",
                "udp_rtp async send to %s:%d pt=%d mtu=%d",
                config->udp_rtp.host, config->udp_rtp.port,
                config->udp_rtp.payload_type, config->udp_rtp.mtu);
  return 0;
}

static int udp_rtp_sink_write(IPC_LITE_STREAM_SINK *sink,
                              const IPC_LITE_STREAM_PACKET *packet) {
  IPC_LITE_UDP_RTP_SINK_CONTEXT *context = sink->ctx;
  uint8_t *new_data = NULL;

  if (!sink->enabled || !context || !packet || packet->len == 0) {
    return 0;
  }

  pthread_mutex_lock(&context->lock);
  if (packet->len > context->pending_capacity) {
    new_data = realloc(context->pending_data, packet->len);
    if (!new_data) {
      pthread_mutex_unlock(&context->lock);
      return -1;
    }
    context->pending_data = new_data;
    context->pending_capacity = packet->len;
  }

  if (context->has_pending) {
    context->queue_drop_count++;
    if (context->queue_drop_count <= 5U) {
      IPC_LITE_LOGW("udp_rtp_sink", "replace pending udp frame, drop=%u",
                    context->queue_drop_count);
    }
  }
  memcpy(context->pending_data, packet->data, packet->len);
  context->pending_len = packet->len;
  context->pending_pts = packet->pts;
  context->pending_key_frame = packet->key_frame;
  context->has_pending = true;
  pthread_cond_signal(&context->cond);
  pthread_mutex_unlock(&context->lock);

  return 0;
}

static void udp_rtp_sink_close(IPC_LITE_STREAM_SINK *sink) {
  IPC_LITE_UDP_RTP_SINK_CONTEXT *context = sink->ctx;

  if (!context) {
    return;
  }

  pthread_mutex_lock(&context->lock);
  context->stop = true;
  context->has_pending = false;
  pthread_cond_signal(&context->cond);
  pthread_mutex_unlock(&context->lock);

  if (context->thread_started) {
    pthread_join(context->thread, NULL);
  }

  if (context->fd >= 0) {
    close(context->fd);
  }

  free(context->pending_data);
  pthread_cond_destroy(&context->cond);
  pthread_mutex_destroy(&context->lock);

  free(context);
  sink->ctx = NULL;
  sink->enabled = false;
}

void ipc_lite_udp_rtp_sink_init(IPC_LITE_STREAM_SINK *sink) {
  memset(sink, 0, sizeof(*sink));
  sink->name = "udp_rtp";
  sink->open = udp_rtp_sink_open;
  sink->write = udp_rtp_sink_write;
  sink->close = udp_rtp_sink_close;
}
