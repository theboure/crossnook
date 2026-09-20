#!/usr/bin/env python3
"""Deterministic local SNTP server for CrossNook host/hardware validation."""

import argparse
import select
import socket
import struct


NTP_UNIX_DELTA = 2_208_988_800
PACKET_BYTES = 48
SUITE_MODES = (
    "valid",
    "originate-mismatch",
    "truncated",
    "oversized",
    "unsynchronized",
    "kiss-of-death",
    "invalid-stratum",
    "zero-receive",
    "zero-transmit",
    "old",
    "future",
    "timeout",
    "wrong-peer",
    "bad-version",
    "bad-mode",
)


def ntp_timestamp(unix_seconds):
    seconds = (unix_seconds + NTP_UNIX_DELTA) & 0xFFFFFFFF
    return struct.pack("!II", seconds, 0)


def response_for(request, mode, timestamp):
    response = bytearray(PACKET_BYTES)
    response[0] = (4 << 3) | 4
    response[1] = 1
    response[24:32] = request[40:48]
    response[32:40] = ntp_timestamp(timestamp - 1)
    response[40:48] = ntp_timestamp(timestamp)

    if mode == "originate-mismatch":
        response[31] ^= 1
    elif mode == "truncated":
        return bytes(response[:-1])
    elif mode == "oversized":
        return bytes(response) + b"X"
    elif mode == "unsynchronized":
        response[0] |= 3 << 6
    elif mode == "kiss-of-death":
        response[1] = 0
    elif mode == "invalid-stratum":
        response[1] = 16
    elif mode == "zero-receive":
        response[32:40] = b"\0" * 8
    elif mode == "zero-transmit":
        response[40:48] = b"\0" * 8
    elif mode == "old":
        response[32:40] = ntp_timestamp(946_857_599)
        response[40:48] = ntp_timestamp(946_857_600)
    elif mode == "future":
        response[32:40] = ntp_timestamp(2_147_483_647)
        response[40:48] = ntp_timestamp(2_147_483_648)
    elif mode == "bad-version":
        response[0] = (response[0] & ~0x38) | (3 << 3)
    elif mode == "bad-mode":
        response[0] = (response[0] & ~0x07) | 5
    return bytes(response)


def bind_udp(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    return sock


def serve(bindings, timestamp, count):
    sockets = []
    modes = {}
    wrong_peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    wrong_peer.bind(("0.0.0.0", 0))
    handled = 0

    for host, port, mode in bindings:
        sock = bind_udp(host, port)
        sockets.append(sock)
        modes[sock] = mode
    print(
        "SNTP MOCK READY "
        + " ".join(f"{sock.getsockname()[1]}={modes[sock]}" for sock in sockets),
        flush=True,
    )

    try:
        while count == 0 or handled < count:
            readable, _, _ = select.select(sockets, [], [], 0.5)
            for sock in readable:
                request, peer = sock.recvfrom(512)
                mode = modes[sock]
                handled += 1
                if mode == "timeout" or len(request) != PACKET_BYTES:
                    continue
                response = response_for(request, mode, timestamp)
                if mode == "wrong-peer":
                    wrong_peer.sendto(response, peer)
                else:
                    sock.sendto(response, peer)
    finally:
        wrong_peer.close()
        for sock in sockets:
            sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19123)
    parser.add_argument("--timestamp", type=int, default=1_790_000_000)
    parser.add_argument("--mode", choices=SUITE_MODES, default="valid")
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--suite", action="store_true")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    if args.count < 0:
        parser.error("count must be nonnegative")
    if args.suite and args.port + len(SUITE_MODES) - 1 > 65535:
        parser.error("suite port range exceeds 65535")

    if args.suite:
        bindings = [
            (args.host, args.port + offset, mode)
            for offset, mode in enumerate(SUITE_MODES)
        ]
    else:
        bindings = [(args.host, args.port, args.mode)]
    serve(bindings, args.timestamp, args.count)


if __name__ == "__main__":
    main()
