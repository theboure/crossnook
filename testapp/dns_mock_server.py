#!/usr/bin/env python3
"""Deterministic UDP DNS mock for CrossNook resolver validation."""

import argparse
import datetime
import select
import socket
import struct
import time


SUITE_MODES = (
    "valid",
    "multiple",
    "duplicate",
    "many",
    "nxdomain",
    "servfail-then-valid",
    "timeout",
    "wrong-id",
    "wrong-source",
    "truncated-packet",
    "oversized",
    "tc",
    "mismatched-question",
    "bad-pointer",
    "pointer-loop",
    "zero-address",
    "unrelated-owner",
    "cname-only",
    "unusual-unrelated-owner",
    "reserved-z-bit",
    "drop-first-two-then-valid",
    "drop-all-fourth-valid",
    "delayed-first-after-two-retransmits",
    "delayed-second-after-retransmit",
)


def encode_name(name):
    wire = bytearray()
    for label in name.split("."):
        encoded = label.encode("ascii")
        wire.append(len(encoded))
        wire.extend(encoded)
    wire.append(0)
    return bytes(wire)


def parse_question(query):
    if len(query) < 17:
        raise ValueError("short query")
    offset = 12
    labels = []
    while True:
        if offset >= len(query):
            raise ValueError("short name")
        length = query[offset]
        offset += 1
        if length == 0:
            break
        if length > 63 or offset + length > len(query):
            raise ValueError("bad label")
        labels.append(query[offset : offset + length].decode("ascii"))
        offset += length
    if offset + 4 != len(query):
        raise ValueError("unexpected query shape")
    return ".".join(labels), query[12 : offset + 4]


def a_record(owner, address, data_length=4):
    data = address[:data_length]
    return owner + struct.pack("!HHIH", 1, 1, 60, data_length) + data


def cname_record(owner):
    target = encode_name("alias.test.local")
    return owner + struct.pack("!HHIH", 5, 1, 60, len(target)) + target


def make_response(query, mode, answer_ip, request_number):
    qname, question = parse_question(query)
    transaction = bytearray(query[:2])
    flags = 0x8180
    answers = []
    response_question = question
    owner = b"\xc0\x0c"
    base = socket.inet_aton(answer_ip)

    if mode == "wrong-id":
        transaction[1] ^= 1
    elif mode == "nxdomain":
        flags = 0x8183
    elif mode == "servfail-then-valid" and request_number == 1:
        flags = 0x8182
    elif mode == "tc":
        flags = 0x8380
    elif mode == "reserved-z-bit":
        flags |= 0x0040
    elif mode == "mismatched-question":
        response_question = encode_name("different.test") + struct.pack("!HH", 1, 1)
    elif mode == "multiple":
        answers = [
            a_record(owner, socket.inet_aton(f"10.20.30.{i}"))
            for i in range(1, 4)
        ]
    elif mode == "duplicate":
        answers = [a_record(owner, base), a_record(owner, base)]
    elif mode == "many":
        answers = [
            a_record(owner, socket.inet_aton(f"10.20.30.{i}"))
            for i in range(1, 7)
        ]
    elif mode == "bad-pointer":
        answers = [a_record(b"\xc0\xff", base)]
    elif mode == "pointer-loop":
        answer_offset = 12 + len(response_question)
        loop_owner = struct.pack("!H", 0xC000 | answer_offset)
        answers = [a_record(loop_owner, base)]
    elif mode == "zero-address":
        answers = []
    elif mode == "unrelated-owner":
        answers = [a_record(encode_name("unrelated.test"), base)]
    elif mode == "cname-only":
        answers = [cname_record(owner)]
    elif mode == "unusual-unrelated-owner":
        answers = [
            a_record(encode_name("_service.test"), socket.inet_aton("10.0.0.1")),
            a_record(owner, base),
        ]
    elif mode not in ("nxdomain", "tc", "mismatched-question") and not (
        mode == "servfail-then-valid" and request_number == 1
    ):
        answers = [a_record(owner, base)]

    packet = (
        bytes(transaction)
        + struct.pack("!HHHHH", flags, 1, len(answers), 0, 0)
        + response_question
        + b"".join(answers)
    )
    if mode == "truncated-packet":
        return packet[:20]
    if mode == "oversized":
        return packet + b"X" * (513 - len(packet))
    return packet


def bind_udp(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    else:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    return sock


def log_event(enabled, event, **fields):
    if not enabled:
        return
    wall = datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")
    details = " ".join(f"{key}={value}" for key, value in fields.items())
    print(
        f"DNS MOCK {event} wall={wall} mono_ns={time.monotonic_ns()} {details}",
        flush=True,
    )


def serve(bindings, answer_ip, count, verbose):
    sockets = []
    modes = {}
    requests = {}
    first_queries = {}
    second_queries = {}
    wrong_source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    wrong_source.bind(("0.0.0.0", 0))
    handled = 0

    for host, port, mode in bindings:
        sock = bind_udp(host, port)
        sockets.append(sock)
        modes[sock] = mode
        requests[sock] = 0
    print(
        "DNS MOCK READY "
        + " ".join(f"{sock.getsockname()[1]}={modes[sock]}" for sock in sockets),
        flush=True,
    )

    try:
        while count == 0 or handled < count:
            readable, _, _ = select.select(sockets, [], [], 0.5)
            for sock in readable:
                query, peer = sock.recvfrom(1024)
                mode = modes[sock]
                requests[sock] += 1
                handled += 1
                transaction = (
                    f"0x{struct.unpack('!H', query[:2])[0]:04x}"
                    if len(query) >= 2
                    else "none"
                )
                try:
                    qname, _ = parse_question(query)
                except (ValueError, UnicodeDecodeError) as error:
                    log_event(
                        verbose,
                        "RX-DROP",
                        seq=handled,
                        peer=f"{peer[0]}:{peer[1]}",
                        bytes=len(query),
                        id=transaction,
                        reason=type(error).__name__,
                        detail=str(error).replace(" ", "_"),
                    )
                    continue
                log_event(
                    verbose,
                    "RX",
                    seq=handled,
                    peer=f"{peer[0]}:{peer[1]}",
                    bytes=len(query),
                    id=transaction,
                    host=qname,
                    mode=mode,
                )
                if mode == "timeout":
                    log_event(verbose, "DROP", seq=handled, reason="configured")
                    continue
                if mode in (
                    "drop-first-two-then-valid",
                    "drop-all-fourth-valid",
                    "delayed-first-after-two-retransmits",
                    "delayed-second-after-retransmit",
                ):
                    if requests[sock] == 1:
                        first_queries[sock] = bytes(query)
                    elif query != first_queries[sock]:
                        print(
                            f"DNS MOCK IDENTITY-ERROR seq={handled} "
                            "retransmission_changed=1",
                            flush=True,
                        )
                        continue
                    if requests[sock] == 2:
                        second_queries[sock] = bytes(query)
                    drop_limit = {
                        "drop-first-two-then-valid": 2,
                        "drop-all-fourth-valid": 3,
                        "delayed-first-after-two-retransmits": 2,
                        "delayed-second-after-retransmit": 2,
                    }[mode]
                    if requests[sock] <= drop_limit:
                        log_event(
                            verbose,
                            "DROP",
                            seq=handled,
                            reason="configured",
                        )
                        continue
                try:
                    response_query = (
                        first_queries[sock]
                        if mode == "delayed-first-after-two-retransmits"
                        and requests[sock] == 3
                        else second_queries[sock]
                        if mode == "delayed-second-after-retransmit"
                        and requests[sock] == 3
                        else query
                    )
                    response = make_response(
                        response_query, mode, answer_ip, requests[sock]
                    )
                except (ValueError, UnicodeDecodeError) as error:
                    log_event(
                        verbose,
                        "DROP",
                        seq=handled,
                        reason=type(error).__name__,
                        detail=str(error).replace(" ", "_"),
                    )
                    continue
                send_socket = wrong_source if mode == "wrong-source" else sock
                send_started = time.monotonic_ns()
                if mode == "wrong-source":
                    source_port = wrong_source.getsockname()[1]
                else:
                    source_port = sock.getsockname()[1]
                try:
                    sent = send_socket.sendto(response, peer)
                except OSError as error:
                    log_event(
                        verbose,
                        "SEND-ERROR",
                        seq=handled,
                        destination=f"{peer[0]}:{peer[1]}",
                        error=repr(error).replace(" ", "_"),
                    )
                    raise
                log_event(
                    verbose,
                    "TX",
                    seq=handled,
                    source_port=source_port,
                    destination=f"{peer[0]}:{peer[1]}",
                    bytes=sent,
                    id=f"0x{struct.unpack('!H', response[:2])[0]:04x}",
                    send_started_ns=send_started,
                )
    finally:
        wrong_source.close()
        for sock in sockets:
            sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19153)
    parser.add_argument("--answer", default="127.0.0.1")
    parser.add_argument("--mode", choices=SUITE_MODES, default="valid")
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--suite", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        socket.inet_aton(args.answer)
    except OSError:
        parser.error("answer must be numeric IPv4")
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
    serve(bindings, args.answer, args.count, args.verbose)


if __name__ == "__main__":
    main()
