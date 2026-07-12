"""Low-overhead operator snapshots for the NDNSF-DI local candidate."""

from __future__ import annotations

import json
import os
import tempfile
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class MetricsSnapshot:
    sampled_at_ms: int = 0
    counters: dict[str, int] = field(default_factory=dict)
    gauges: dict[str, float] = field(default_factory=dict)
    labels: dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "MetricsSnapshot":
        return cls(
            sampled_at_ms=int(value.get("sampledAtMs", value.get("sampled_at_ms", 0)) or 0),
            counters={str(k): int(v) for k, v in value.get("counters", {}).items()},
            gauges={str(k): float(v) for k, v in value.get("gauges", {}).items()},
            labels={str(k): str(v) for k, v in value.get("labels", {}).items()},
        )

    def to_dict(self) -> dict[str, Any]:
        payload = asdict(self)
        payload["schema"] = "ndnsf-di-metrics-v1"
        payload["sampledAtMs"] = payload.pop("sampled_at_ms") or int(time.time() * 1000)
        return payload


def _prometheus(snapshot: MetricsSnapshot) -> str:
    label_text = ",".join(
        f'{key}="{value.replace(chr(92), "").replace(chr(34), chr(92) + chr(34))}"'
        for key, value in sorted(snapshot.labels.items()))
    suffix = "{" + label_text + "}" if label_text else ""
    rows = []
    for kind, values in (("counter", snapshot.counters), ("gauge", snapshot.gauges)):
        for name, value in sorted(values.items()):
            safe = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in name)
            metric = f"ndnsf_di_{safe}"
            rows.extend((f"# TYPE {metric} {kind}", f"{metric}{suffix} {value}"))
    return "\n".join(rows) + ("\n" if rows else "")


def atomic_export_metrics(snapshot: MetricsSnapshot, output: str | Path,
                          format_name: str = "json") -> None:
    destination = Path(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if format_name == "json":
        content = json.dumps(snapshot.to_dict(), indent=2, sort_keys=True) + "\n"
    elif format_name == "prometheus-textfile":
        content = _prometheus(snapshot)
    else:
        raise ValueError(f"unsupported metrics format: {format_name}")
    fd, temp_name = tempfile.mkstemp(prefix=f".{destination.name}.", dir=destination.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, destination)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise
