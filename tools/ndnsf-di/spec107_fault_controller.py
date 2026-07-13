#!/usr/bin/env python3
"""Campaign-owned process controller for Spec 107 live fault experiments."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from typing import Sequence

from spec107_lineage import sha256_file

import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.runtime_v1_evidence import OwnedProcessV1  # noqa: E402


class FaultControllerError(RuntimeError):
    """Stable refusal from the process-ownership boundary."""


def _fail(code: str, detail: str = "") -> None:
    raise FaultControllerError(code + (f":{detail}" if detail else ""))


def _digest_argv(argv: Sequence[str]) -> str:
    encoded = json.dumps(list(argv), separators=(",", ":"), ensure_ascii=True).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _proc_stat(pid: int) -> tuple[int, int, int]:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except OSError as exc:
        _fail("OWNED_PROCESS_NOT_LIVE", str(pid))
    close = text.rfind(")")
    if close < 0:
        _fail("OWNED_PROCESS_PROC_INVALID", str(pid))
    fields = text[close + 2:].split()
    if len(fields) < 20:
        _fail("OWNED_PROCESS_PROC_INVALID", str(pid))
    return int(fields[1]), os.getpgid(pid), int(fields[19])


def _proc_argv(pid: int) -> list[str]:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        _fail("OWNED_PROCESS_NOT_LIVE", str(pid))
    return [part.decode("utf-8", "surrogateescape") for part in raw.split(b"\0") if part]


def _proc_executable(pid: int) -> Path:
    try:
        return Path(os.readlink(f"/proc/{pid}/exe")).resolve()
    except OSError:
        _fail("OWNED_PROCESS_NOT_LIVE", str(pid))


class OwnedProcessRegistry:
    """Own and mutate only exact child identities in one fault campaign."""

    def __init__(self, *, campaign_id: str, registry_path: Path | str) -> None:
        if not campaign_id.startswith("spec107-c1-fault-") or "spec105" in campaign_id.lower():
            _fail("OWNED_PROCESS_CAMPAIGN_INVALID")
        self.campaign_id = campaign_id
        self.registry_path = Path(registry_path)
        self._entries: dict[int, tuple[OwnedProcessV1, list[str], subprocess.Popen[bytes]]] = {}
        self._replacement_by_role: dict[str, int] = {}

    def _persist(self) -> None:
        self.registry_path.parent.mkdir(parents=True, exist_ok=True)
        value = {
            "schema": "ndnsf-di-spec107-owned-process-registry-v1",
            "campaignId": self.campaign_id,
            "processes": [entry[0].to_dict() for entry in self._entries.values()],
        }
        temporary = self.registry_path.with_suffix(self.registry_path.suffix + ".tmp")
        temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, self.registry_path)

    def launch(self, command: Sequence[str], *, role: str, provider_name: str,
               provider_boot_id: str) -> OwnedProcessV1:
        argv = [str(item) for item in command]
        if not argv or not role or not provider_name or not provider_boot_id:
            _fail("OWNED_PROCESS_LAUNCH_INVALID")
        process = subprocess.Popen(argv, start_new_session=True)
        try:
            deadline = time.monotonic() + 2.0
            while not Path(f"/proc/{process.pid}/stat").exists() and time.monotonic() < deadline:
                time.sleep(0.005)
            parent_pid, process_group, start_ticks = _proc_stat(process.pid)
            live_argv = _proc_argv(process.pid)
            executable = _proc_executable(process.pid)
            owned = OwnedProcessV1(
                pid=process.pid,
                process_group_id=process_group,
                proc_start_time_ticks=start_ticks,
                parent_pid=parent_pid,
                campaign_id=self.campaign_id,
                role=role,
                provider_name=provider_name,
                provider_boot_id=provider_boot_id,
                command_digest=_digest_argv(live_argv),
                executable_digest=sha256_file(executable),
            )
            self._entries[owned.pid] = (owned, argv, process)
            self._persist()
            return owned
        except Exception:
            process.terminate()
            process.wait(timeout=2)
            raise

    def adopt(self, process: subprocess.Popen[bytes], *, role: str,
              provider_name: str, provider_boot_id: str) -> OwnedProcessV1:
        if not role or not provider_name or not provider_boot_id:
            _fail("OWNED_PROCESS_ADOPTION_INVALID")
        parent_pid, process_group, start_ticks = _proc_stat(process.pid)
        if process_group != process.pid:
            _fail("OWNED_PROCESS_GROUP_NOT_EXCLUSIVE", str(process.pid))
        live_argv = _proc_argv(process.pid)
        executable = _proc_executable(process.pid)
        owned = OwnedProcessV1(
            pid=process.pid, process_group_id=process_group,
            proc_start_time_ticks=start_ticks, parent_pid=parent_pid,
            campaign_id=self.campaign_id, role=role,
            provider_name=provider_name, provider_boot_id=provider_boot_id,
            command_digest=_digest_argv(live_argv),
            executable_digest=sha256_file(executable),
        )
        self._entries[owned.pid] = (owned, live_argv, process)
        self._persist()
        return owned

    def _require_exact(self, target: OwnedProcessV1) -> tuple[OwnedProcessV1, list[str], subprocess.Popen[bytes]]:
        if target.campaign_id != self.campaign_id:
            _fail("OWNED_PROCESS_CAMPAIGN_MISMATCH")
        entry = self._entries.get(target.pid)
        if entry is None:
            _fail("OWNED_PROCESS_REGISTRY_MISMATCH")
        expected, argv, process = entry
        comparisons = {
            "REGISTRY": expected.to_dict() == target.to_dict(),
            "PARENT": _proc_stat(target.pid)[0] == target.parent_pid,
            "PROCESS_GROUP": _proc_stat(target.pid)[1] == target.process_group_id,
            "START_TIME": _proc_stat(target.pid)[2] == target.proc_start_time_ticks,
            "COMMAND": _digest_argv(_proc_argv(target.pid)) == target.command_digest,
            "EXECUTABLE": sha256_file(_proc_executable(target.pid)) == target.executable_digest,
        }
        for name, matched in comparisons.items():
            if not matched:
                _fail(f"OWNED_PROCESS_{name}_MISMATCH", str(target.pid))
        return expected, argv, process

    def guarded_signal(self, target: OwnedProcessV1, signal_number: int) -> None:
        self._require_exact(target)
        os.killpg(target.process_group_id, signal_number)

    def wait_for_log_trigger(self, target: OwnedProcessV1, *, log_path: Path | str,
                             marker: str, timeout_seconds: float) -> int:
        self._require_exact(target)
        if not marker or timeout_seconds <= 0:
            _fail("FAULT_TRIGGER_INVALID")
        deadline = time.monotonic() + timeout_seconds
        path = Path(log_path)
        while time.monotonic() < deadline:
            try:
                if marker in path.read_text(encoding="utf-8", errors="replace"):
                    return time.monotonic_ns() // 1000
            except FileNotFoundError:
                pass
            time.sleep(0.01)
        _fail("FAULT_TRIGGER_NOT_OBSERVED", marker)

    def observe_process_exit(self, target: OwnedProcessV1,
                             *, timeout_seconds: float) -> dict[str, object]:
        entry = self._entries.get(target.pid)
        if entry is None or entry[0].to_dict() != target.to_dict():
            _fail("OWNED_PROCESS_REGISTRY_MISMATCH")
        _expected, _argv, process = entry
        try:
            return_code = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            _fail("FAULT_EFFECT_NOT_OBSERVED", str(target.pid))
        self._entries.pop(target.pid, None)
        self._persist()
        return {
            "observed": True,
            "effect": "provider-process-exit",
            "pid": target.pid,
            "returnCode": return_code,
            "observedMonotonicUs": time.monotonic_ns() // 1000,
        }

    def restart(self, target: OwnedProcessV1, *, provider_boot_id: str) -> OwnedProcessV1:
        expected, argv, process = self._require_exact(target)
        if self._replacement_by_role.get(expected.role, 0) >= 1:
            _fail("OWNED_PROCESS_REPLACEMENT_BOUND", expected.role)
        os.killpg(expected.process_group_id, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(expected.process_group_id, signal.SIGKILL)
            process.wait(timeout=2)
        self._entries.pop(expected.pid, None)
        self._replacement_by_role[expected.role] = 1
        return self.launch(argv, role=expected.role, provider_name=expected.provider_name,
                           provider_boot_id=provider_boot_id)

    def cleanup(self) -> dict[str, object]:
        failures: list[int] = []
        for pid, (owned, _argv, process) in list(self._entries.items()):
            try:
                self._require_exact(owned)
                os.killpg(owned.process_group_id, signal.SIGTERM)
                process.wait(timeout=5)
            except ProcessLookupError:
                pass
            except Exception:
                failures.append(pid)
            self._entries.pop(pid, None)
        self._persist()
        return {"proven": not failures, "remainingPids": failures}


__all__ = ["FaultControllerError", "OwnedProcessRegistry"]
