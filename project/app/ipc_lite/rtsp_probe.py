#!/usr/bin/env python3
import select
import socket
import sys
import time


def recv_rtsp_response(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        data += sock.recv(4096)
    header, body = data.split(b"\r\n\r\n", 1)
    content_length = 0
    for line in header.decode("latin1", "ignore").split("\r\n"):
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
            break
    while len(body) < content_length:
        body += sock.recv(4096)
    return (header + b"\r\n\r\n" + body[:content_length]).decode("latin1", "ignore")


def send(sock, request):
    sock.sendall(request.encode("latin1"))
    response = recv_rtsp_response(sock)
    print(response)
    print("-----")
    return response


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "172.20.10.12"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 554
    mode = sys.argv[3].lower() if len(sys.argv) > 3 else "tcp"
    url = f"rtsp://{host}:{port}/live/0"

    sock = socket.create_connection((host, port), timeout=5)
    sock.settimeout(5)

    send(sock, f"OPTIONS {url} RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: pyprobe\r\n\r\n")
    describe = send(
        sock,
        f"DESCRIBE {url} RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n"
        f"User-Agent: pyprobe\r\n\r\n",
    )

    track = "track1"
    for line in describe.split("\r\n"):
        if line.startswith("a=control:"):
            value = line.split(":", 1)[1].strip()
            if value and not value.startswith("*") and "track" in value:
                track = value

    setup_url = track if track.startswith("rtsp://") else f"{url}/{track}"
    rtp_sock = None
    rtcp_sock = None
    rtp_port = 0
    rtcp_port = 0

    if mode == "udp":
        rtp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtcp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        bound = False
        for base_port in range(50000, 51000, 2):
            try:
                rtp_sock.bind(("0.0.0.0", base_port))
                rtcp_sock.bind(("0.0.0.0", base_port + 1))
                rtp_port = base_port
                rtcp_port = base_port + 1
                bound = True
                break
            except OSError:
                try:
                    rtp_sock.close()
                except OSError:
                    pass
                try:
                    rtcp_sock.close()
                except OSError:
                    pass
                rtp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                rtcp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if not bound:
            raise RuntimeError("failed to bind consecutive UDP ports for RTP/RTCP")
        rtp_sock.setblocking(False)
        rtcp_sock.setblocking(False)
        transport_line = (
            f"Transport: RTP/AVP;unicast;client_port={rtp_port}-{rtcp_port}\r\n\r\n"
        )
    else:
        transport_line = (
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n"
        )

    setup = send(
        sock,
        f"SETUP {setup_url} RTSP/1.0\r\nCSeq: 3\r\nUser-Agent: pyprobe\r\n"
        f"{transport_line}",
    )

    session = ""
    for line in setup.split("\r\n"):
        if line.startswith("Session:"):
            session = line.split(":", 1)[1].strip().split(";", 1)[0]
            break

    send(
        sock,
        f"PLAY {url} RTSP/1.0\r\nCSeq: 4\r\nUser-Agent: pyprobe\r\n"
        f"Session: {session}\r\nRange: npt=0.000-\r\n\r\n",
    )

    packets = 0
    payload_bytes = 0
    if mode == "udp":
        end = time.time() + 3
        while time.time() < end:
            readable, _, _ = select.select([rtp_sock, rtcp_sock], [], [], 1.0)
            for udp_sock in readable:
                chunk, _ = udp_sock.recvfrom(8192)
                if udp_sock is rtp_sock:
                    packets += 1
                    payload_bytes += len(chunk)
        print(
            f"RTP UDP packets: {packets}, payload bytes: {payload_bytes}, "
            f"client_port={rtp_port}-{rtcp_port}"
        )
        rtp_sock.close()
        rtcp_sock.close()
    else:
        sock.settimeout(1)
        end = time.time() + 3
        while time.time() < end:
            try:
                chunk = sock.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                break
            i = 0
            while i + 4 <= len(chunk):
                if chunk[i] == 0x24:
                    length = (chunk[i + 2] << 8) | chunk[i + 3]
                    if i + 4 + length <= len(chunk):
                        packets += 1
                        payload_bytes += length
                        i += 4 + length
                    else:
                        break
                else:
                    i += 1
        print(f"RTP TCP interleaved packets: {packets}, payload bytes: {payload_bytes}")
    sock.close()


if __name__ == "__main__":
    main()
