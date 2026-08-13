#!/usr/bin/env python3
"""Generate the deterministic Spec 171 Provider-transition availability trace."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


PROVIDERS = ("ucla", "wustl", "uiuc", "arizona")
AP_X = 200.0
AP_Y = 200.0
IN_POSITION = (200.0, 200.0, 0.0)
OUT_POSITION = (0.0, 0.0, 282.84)


def availability(time_s: float, join_s: float, retire_s: float) -> dict[str, bool]:
    if time_s < join_s:
        return {name: name != "arizona" for name in PROVIDERS}
    if time_s < retire_s:
        return {name: True for name in PROVIDERS}
    return {name: name == "arizona" for name in PROVIDERS}


def generate(
        output: Path, *, duration_s: float, step_s: float,
        join_s: float, retire_s: float) -> int:
    if not (0 < join_s < retire_s < duration_s):
        raise ValueError("require 0 < join_s < retire_s < duration_s")
    if step_s <= 0:
        raise ValueError("step_s must be positive")
    output.parent.mkdir(parents=True, exist_ok=True)
    count = int(round(duration_s / step_s)) + 1
    with output.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow((
            "time_s", "provider", "x", "y", "distance_m", "in_range",
            "nearest_ap"))
        for index in range(count):
            time_s = min(duration_s, index * step_s)
            state = availability(time_s, join_s, retire_s)
            for provider in PROVIDERS:
                x, y, distance = IN_POSITION if state[provider] else OUT_POSITION
                writer.writerow((
                    f"{time_s:.3f}", provider, f"{x:.2f}", f"{y:.2f}",
                    f"{distance:.2f}", int(state[provider]), "ap1"))
    return count * len(PROVIDERS)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--duration-s", type=float, default=70.0)
    parser.add_argument("--step-s", type=float, default=0.1)
    parser.add_argument("--join-s", type=float, default=20.0)
    parser.add_argument("--retire-s", type=float, default=40.0)
    args = parser.parse_args()
    rows = generate(
        args.output, duration_s=args.duration_s, step_s=args.step_s,
        join_s=args.join_s, retire_s=args.retire_s)
    print(f"wrote {rows} rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
