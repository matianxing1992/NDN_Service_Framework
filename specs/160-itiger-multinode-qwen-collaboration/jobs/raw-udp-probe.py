#!/usr/bin/env python3
import socket
import sys
import time
from pathlib import Path


def listen(bind_host: str, port: int, expected: str, ready_path: str) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_host, port))
    sock.settimeout(120.0)
    Path(ready_path).write_text("ready\n", encoding="utf-8")
    deadline = time.time() + 120.0
    while time.time() < deadline:
        try:
            payload, addr = sock.recvfrom(2048)
        except socket.timeout:
            break
        text = payload.decode("utf-8", errors="replace")
        print(f"SPEC160_RAW_UDP_RECV from={addr[0]}:{addr[1]} payload={text}", flush=True)
        if text == expected:
            print("SPEC160_RAW_UDP_PASS", flush=True)
            return 0
    print(f"SPEC160_RAW_UDP_TIMEOUT expected={expected}", file=sys.stderr, flush=True)
    return 4


def send(host: str, port: int, payload: str) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data = payload.encode("utf-8")
    for attempt in range(1, 6):
        sock.sendto(data, (host, port))
        print(f"SPEC160_RAW_UDP_SEND attempt={attempt} to={host}:{port} payload={payload}", flush=True)
        time.sleep(0.2)
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: raw-udp-probe.py listen|send ...", file=sys.stderr)
        return 2
    mode = sys.argv[1]
    if mode == "listen":
        if len(sys.argv) != 6:
            print("usage: raw-udp-probe.py listen BIND_HOST PORT EXPECTED READY_PATH", file=sys.stderr)
            return 2
        return listen(sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5])
    if mode == "send":
        if len(sys.argv) != 5:
            print("usage: raw-udp-probe.py send HOST PORT PAYLOAD", file=sys.stderr)
            return 2
        return send(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    print(f"unknown mode: {mode}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
