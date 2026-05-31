#!/usr/bin/env python3
"""Run one low-rate YOLO detection request for NDNSF-UAV-APP.

The ground-station app invokes this helper for live-drone snapshots. It keeps
the C++ service generic while using the locally installed Ultralytics stack when
available.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def encode_fields(fields: dict[str, str]) -> str:
    return ";".join(f"{k}={str(v).replace(';', ',')}" for k, v in fields.items())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--image", required=True)
    parser.add_argument("--conf", type=float, default=0.25)
    parser.add_argument("--classes", default="car,truck")
    args = parser.parse_args()

    image = Path(args.image)
    if not image.exists():
      print(encode_fields({"ok": "false", "error": f"image not found: {image}"}))
      return 2

    try:
        from ultralytics import YOLO
    except Exception as exc:  # pragma: no cover - depends on deployment
        print(encode_fields({"ok": "false", "error": f"ultralytics unavailable: {exc}"}))
        return 3

    wanted = {item.strip().lower() for item in args.classes.split(",") if item.strip()}
    try:
        model = YOLO(args.model)
        results = model.predict(str(image), conf=args.conf, verbose=False, device="cpu")
    except Exception as exc:
        print(encode_fields({"ok": "false", "error": f"yolo failed: {exc}"}))
        return 4

    counts: dict[str, int] = {name: 0 for name in wanted}
    best_conf: dict[str, float] = {name: 0.0 for name in wanted}
    names = getattr(model, "names", {})
    for result in results:
        boxes = getattr(result, "boxes", None)
        if boxes is None:
            continue
        for box in boxes:
            cls_id = int(box.cls[0])
            cls_name = str(names.get(cls_id, cls_id)).lower()
            if cls_name not in wanted:
                continue
            conf = float(box.conf[0])
            counts[cls_name] += 1
            best_conf[cls_name] = max(best_conf[cls_name], conf)

    objects = [name.capitalize() for name, count in counts.items() if count > 0]
    fields = {
        "ok": "true",
        "objects": ",".join(objects) if objects else "none",
        "car": "true" if counts.get("car", 0) > 0 else "false",
        "truck": "true" if counts.get("truck", 0) > 0 else "false",
        "car_count": str(counts.get("car", 0)),
        "truck_count": str(counts.get("truck", 0)),
        "car_conf": f"{best_conf.get('car', 0.0):.3f}",
        "truck_conf": f"{best_conf.get('truck', 0.0):.3f}",
        "model": str(args.model),
    }
    print(encode_fields(fields))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
