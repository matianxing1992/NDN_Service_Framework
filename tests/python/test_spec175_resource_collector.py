from __future__ import annotations

import importlib.util
from pathlib import Path
import types


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/collect_spec175_resources.py"
SPEC = importlib.util.spec_from_file_location("spec175_resources", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
COLLECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECTOR)


def test_descendant_accounting_is_bounded_to_runner_tree():
    rows = {
        10: (1, 100, 1),
        11: (10, 200, 2),
        12: (11, 300, 3),
        20: (1, 999, 9),
    }
    assert COLLECTOR._descendants(10, rows) == {10, 11, 12}


def test_nvidia_smi_parser_retains_three_gpu_identities(monkeypatch):
    completed = types.SimpleNamespace(
        returncode=0, stderr="",
        stdout=(
            "0, GPU-a, 75, 20000, 24576, 180\n"
            "1, GPU-b, 80, 21000, 24576, 190\n"
            "2, GPU-c, 70, 19900, 24576, 175\n"),
    )
    monkeypatch.setattr(
        COLLECTOR.subprocess, "run", lambda *args, **kwargs: completed)
    rows, error = COLLECTOR._gpus()
    assert error == ""
    assert [row["uuid"] for row in rows] == ["GPU-a", "GPU-b", "GPU-c"]
    assert rows[1]["memoryUsedMiB"] == 21000.0
