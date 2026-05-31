#!/usr/bin/env python3
"""Long-lived low-rate YOLO worker for NDNSF-UAV-APP.

The C++ ground-station process sends one image path per stdin line and receives
one key=value result line per image. Keeping the worker alive avoids reloading
the model for every 1 Hz object-detection request.
"""

from __future__ import annotations

import argparse
import contextlib
from pathlib import Path
import sys


def encode_fields(fields: dict[str, str]) -> str:
    return ";".join(f"{k}={str(v).replace(';', ',')}" for k, v in fields.items())


def detect(model, image: Path, wanted: set[str], conf: float) -> dict[str, str]:
    if not image.exists():
        return {"ok": "false", "error": f"image not found: {image}", "objects": "none"}

    try:
        with contextlib.redirect_stdout(sys.stderr):
            results = model.predict(str(image), conf=conf, verbose=False, device="cpu")
    except Exception as exc:
        return {"ok": "false", "error": f"yolo failed: {exc}", "objects": "none"}

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
            score = float(box.conf[0])
            counts[cls_name] += 1
            best_conf[cls_name] = max(best_conf[cls_name], score)

    objects = [name.capitalize() for name, count in counts.items() if count > 0]
    return {
        "ok": "true",
        "objects": ",".join(objects) if objects else "none",
        "car": "true" if counts.get("car", 0) > 0 else "false",
        "truck": "true" if counts.get("truck", 0) > 0 else "false",
        "car_count": str(counts.get("car", 0)),
        "truck_count": str(counts.get("truck", 0)),
        "car_conf": f"{best_conf.get('car', 0.0):.3f}",
        "truck_conf": f"{best_conf.get('truck', 0.0):.3f}",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--conf", type=float, default=0.25)
    parser.add_argument("--classes", default="car,truck")
    args = parser.parse_args()

    wanted = {item.strip().lower() for item in args.classes.split(",") if item.strip()}
    try:
        with contextlib.redirect_stdout(sys.stderr):
            from ultralytics import YOLO
            model = YOLO(args.model)
    except Exception as exc:  # pragma: no cover - depends on deployment
        print(encode_fields({"ready": "false", "error": f"ultralytics unavailable: {exc}"}), flush=True)
        return 3

    print(encode_fields({"ready": "true", "model": str(args.model)}), flush=True)
    for raw_line in sys.stdin:
        image_path = raw_line.strip()
        if not image_path:
            continue
        if image_path == "__quit__":
            break
        fields = detect(model, Path(image_path), wanted, args.conf)
        fields["model"] = str(args.model)
        print(encode_fields(fields), flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
