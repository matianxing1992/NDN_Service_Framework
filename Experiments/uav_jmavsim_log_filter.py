#!/usr/bin/env python3
"""Keep PX4/jMAVSim demo logs useful without letting prompt spam fill a VM."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


MAX_BYTES = int(os.environ.get("NDNSF_UAV_JMAVSIM_LOG_MAX_BYTES", str(8 * 1024 * 1024)))
PROMPT_RE = re.compile(r"^(?:\x1b\[[0-9;]*[A-Za-z])*pxh>\s*$")
STATUS_FILE = os.environ.get("NDNSF_UAV_JMAVSIM_STATUS_FILE", "")


def write_status(value: str) -> None:
    if not STATUS_FILE:
        return
    try:
        Path(STATUS_FILE).write_text(value + "\n", encoding="utf-8")
    except OSError:
        pass


def update_status_from_line(line: str) -> None:
    if "Ready for takeoff!" in line:
        write_status("ready for takeoff")
    elif "Armed by" in line:
        write_status("armed")
    elif "Takeoff detected" in line:
        write_status("takeoff detected")
    elif "Landing detected" in line:
        write_status("landed")
    elif "Disarmed" in line:
        write_status("disarmed")
    elif "Startup script returned successfully" in line:
        write_status("px4 started")
    elif "Simulator connected" in line:
        write_status("simulator connected")


def main() -> int:
    written = 0
    truncated = False
    write_status("starting")
    for raw in sys.stdin.buffer:
        # PX4 repeatedly writes carriage-return prompt updates. Treat them as
        # line boundaries so prompt-only updates can be dropped.
        for part in raw.replace(b"\r", b"\n").splitlines(keepends=True):
            text = part.decode("utf-8", errors="replace")
            clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text).strip()
            if not clean or PROMPT_RE.match(clean):
                continue
            update_status_from_line(clean)
            data = (clean + "\n").encode("utf-8", errors="replace")
            if written + len(data) > MAX_BYTES:
                if not truncated:
                    msg = (
                        f"\n[ndnsf-uav] jMAVSim log truncated at {MAX_BYTES} bytes; "
                        "PX4/jMAVSim is still running.\n"
                    ).encode("utf-8")
                    sys.stdout.buffer.write(msg)
                    sys.stdout.buffer.flush()
                    truncated = True
                continue
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            written += len(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
