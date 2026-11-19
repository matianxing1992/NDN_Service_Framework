"""Explicit external input shared by the local YOLO export regressions."""
from pathlib import Path
import os


def declared_yolo_checkpoint() -> Path:
    value = os.environ.get("SPEC180_YOLO_CHECKPOINT", "")
    if not value:
        raise ValueError("SPEC180_YOLO_CHECKPOINT_REQUIRED")
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise ValueError("SPEC180_YOLO_CHECKPOINT_NOT_ABSOLUTE")
    if not path.is_file():
        raise ValueError("SPEC180_YOLO_CHECKPOINT_NOT_FILE")
    return path
