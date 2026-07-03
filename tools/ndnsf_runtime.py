#!/usr/bin/env python3
"""NDNSF runtime profile, doctor, and structured event helper.

This tool is intentionally stdlib-only so it can run inside MiniNDN nodes and
fresh VMs before optional Python dependencies are installed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

TOKEN_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"
DEFAULT_PROFILE = Path("examples/hello.runtime.json")


def now_ms() -> int:
    return int(time.time() * 1000)


class EventWriter:
    def __init__(self, path: str | Path | None = None) -> None:
        self.path = Path(path) if path else None
        self._fh = None
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._fh = self.path.open("a", encoding="utf-8")

    def emit(self, event: str, **fields: Any) -> None:
        record = {"tsMs": now_ms(), "event": event, **fields}
        line = json.dumps(record, sort_keys=True)
        if self._fh:
            self._fh.write(line + "\n")
            self._fh.flush()
        else:
            print(line, file=sys.stderr)

    def close(self) -> None:
        if self._fh:
            self._fh.close()

    def __enter__(self) -> "EventWriter":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


@dataclass
class RuntimeProfile:
    name: str = "hello"
    controller_prefix: str = "/example/hello/controller"
    policy_file: str = "examples/hello.policies"
    trust_schema: str = "examples/trust-schema.conf"
    token_file: str = "examples/hello.bootstrap-tokens"
    provider_identity: str = "/example/hello/provider"
    user_identity: str = "/example/hello/user"
    service_name: str = "/HELLO"
    env: dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_json(cls, path: str | Path) -> "RuntimeProfile":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        controller = data.get("controller", {})
        provider = data.get("provider", {})
        user = data.get("user", {})
        return cls(
            name=data.get("name", "hello"),
            controller_prefix=controller.get("prefix", data.get("controller_prefix", cls.controller_prefix)),
            policy_file=controller.get("policy_file", data.get("policy_file", cls.policy_file)),
            trust_schema=controller.get("trust_schema", data.get("trust_schema", cls.trust_schema)),
            token_file=controller.get("bootstrap_token_file", data.get("token_file", cls.token_file)),
            provider_identity=provider.get("identity", data.get("provider_identity", cls.provider_identity)),
            user_identity=user.get("identity", data.get("user_identity", cls.user_identity)),
            service_name=data.get("service_name", cls.service_name),
            env={str(k): str(v) for k, v in data.get("env", {}).items()},
        )

    def resolved(self, repo_root: Path) -> dict[str, Any]:
        def abs_path(value: str) -> str:
            path = Path(value)
            return str(path if path.is_absolute() else repo_root / path)

        return {
            "name": self.name,
            "controller": {
                "prefix": self.controller_prefix,
                "policy_file": abs_path(self.policy_file),
                "trust_schema": abs_path(self.trust_schema),
                "bootstrap_token_file": abs_path(self.token_file),
            },
            "provider": {"identity": self.provider_identity},
            "user": {"identity": self.user_identity},
            "service_name": self.service_name,
            "env": self.env,
        }


def repo_root_from(start: Path) -> Path:
    cur = start.resolve()
    for candidate in [cur, *cur.parents]:
        if (candidate / "README.md").exists() and (candidate / "ndn-service-framework").is_dir():
            return candidate
    raise RuntimeError(f"Cannot locate NDNSF repository root from {start}")


def parse_policy_identities(policy_file: Path) -> list[tuple[str, str]]:
    text = policy_file.read_text(encoding="utf-8")
    entries: list[tuple[str, str]] = []
    for section, role in [("provider-policy", "provider"), ("user-policy", "user")]:
        pattern = re.compile(rf"{re.escape(section)}\s*\{{.*?\bfor\s+(/[^\s{{}}]+)", re.S)
        for match in pattern.finditer(text):
            entries.append((match.group(1), role))
    dedup: dict[str, str] = {}
    for identity, role in entries:
        dedup.setdefault(identity, role)
    return sorted(dedup.items())


def generate_token(length: int = 8) -> str:
    return "".join(secrets.choice(TOKEN_ALPHABET) for _ in range(length))


def ensure_token_file(policy_file: Path, token_file: Path, events: EventWriter, fix: bool) -> dict[str, Any]:
    status: dict[str, Any] = {"path": str(token_file), "exists": token_file.exists(), "generated": False}
    if token_file.exists():
        entries = read_token_file(token_file)
        bad = [entry for entry in entries if len(entry[1]) != 8]
        status.update({"entry_count": len(entries), "bad_token_count": len(bad)})
        events.emit("TOKEN_FILE_LOADED", path=str(token_file), entryCount=len(entries), badTokenCount=len(bad))
        return status

    identities = parse_policy_identities(policy_file)
    status.update({"entry_count": len(identities), "bad_token_count": 0})
    if not fix:
        events.emit("TOKEN_FILE_MISSING", path=str(token_file), policy=str(policy_file), identities=len(identities))
        return status

    token_file.parent.mkdir(parents=True, exist_ok=True)
    with token_file.open("w", encoding="utf-8") as fh:
        fh.write("# identity token role\n")
        for identity, role in identities:
            fh.write(f"{identity} {generate_token(8)} {role}\n")
    try:
        token_file.chmod(0o600)
    except OSError:
        pass
    status.update({"exists": True, "generated": True})
    events.emit("TOKEN_FILE_GENERATED", path=str(token_file), entryCount=len(identities), tokenLength=8)
    return status


def read_token_file(path: Path) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            entries.append((parts[0], parts[1], parts[2] if len(parts) >= 3 else ""))
    return entries


def check_nfd_socket(path: Path = Path("/run/nfd/nfd.sock")) -> dict[str, Any]:
    exists = path.exists()
    is_socket = path.is_socket() if exists else False
    return {"path": str(path), "exists": exists, "is_socket": is_socket, "ready": exists and is_socket}


def check_binaries(repo_root: Path, names: Iterable[str]) -> dict[str, bool]:
    result: dict[str, bool] = {}
    for name in names:
        result[name] = (repo_root / "build" / "examples" / name).exists()
    return result


def command_status(command: list[str]) -> dict[str, Any]:
    try:
        proc = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        return {"ok": proc.returncode == 0, "returncode": proc.returncode, "stdout": proc.stdout.strip(), "stderr": proc.stderr.strip()}
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"ok": False, "error": str(exc)}


def summarize_log_markers(log_dir: Path) -> dict[str, list[str]]:
    markers = {
        "bootstrap_issued": "NDNSF_CERT_BOOTSTRAP_ISSUED",
        "bootstrap_reused": "NDNSF_CERT_BOOTSTRAP_REUSED",
        "bootstrap_refused": "NDNSF_CERT_BOOTSTRAP_REFUSED",
        "permission_ready": "Installed user permission",
        "provider_permission_ready": "Installed provider permission",
        "dkey_ready": "DK_DECRYPT_SUCCESS",
        "response_received": "Received response:",
        "negative_ack": "NEGATIVE_ACK",
    }
    result: dict[str, list[str]] = {key: [] for key in markers}
    if not log_dir.exists():
        return result
    for path in sorted(log_dir.glob("*.log")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for key, marker in markers.items():
            if any(marker in line for line in lines):
                result[key].append(path.name)
    return result


def run_doctor(args: argparse.Namespace) -> int:
    repo_root = repo_root_from(Path.cwd())
    profile = RuntimeProfile.from_json(args.profile)
    resolved = profile.resolved(repo_root)
    with EventWriter(args.event_log) as events:
        events.emit("DOCTOR_START", profile=profile.name, repoRoot=str(repo_root))
        policy_file = Path(resolved["controller"]["policy_file"])
        token_file = Path(resolved["controller"]["bootstrap_token_file"])
        checks: dict[str, Any] = {"profile": resolved}
        checks["policy_identities"] = parse_policy_identities(policy_file) if policy_file.exists() else []
        checks["token_file"] = ensure_token_file(policy_file, token_file, events, args.fix)
        checks["nfd"] = check_nfd_socket()
        checks["binaries"] = check_binaries(repo_root, ["App_ServiceController", "App_Provider", "App_User"])
        checks["commands"] = {
            "nfdc": command_status(["nfdc", "status", "show"]),
        } if args.check_commands else {}
        checks["log_summary"] = summarize_log_markers(Path(args.log_dir)) if args.log_dir else {}

        ready = True
        ready = ready and Path(resolved["controller"]["policy_file"]).exists()
        ready = ready and Path(resolved["controller"]["trust_schema"]).exists()
        ready = ready and checks["token_file"].get("exists", False)
        ready = ready and all(checks["binaries"].values())
        checks["ready"] = bool(ready)
        events.emit("DOCTOR_RESULT", ready=bool(ready), tokenFile=checks["token_file"], nfd=checks["nfd"], binaries=checks["binaries"])

        if args.write_resolved:
            Path(args.write_resolved).parent.mkdir(parents=True, exist_ok=True)
            Path(args.write_resolved).write_text(json.dumps(checks, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(checks, indent=2, sort_keys=True))
        return 0 if ready else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="NDNSF runtime profile doctor")
    sub = parser.add_subparsers(dest="command", required=True)
    doctor = sub.add_parser("doctor", help="validate a runtime profile and emit structured events")
    doctor.add_argument("--profile", default=str(DEFAULT_PROFILE))
    doctor.add_argument("--fix", action="store_true", help="create missing generated files such as bootstrap tokens")
    doctor.add_argument("--event-log", default="")
    doctor.add_argument("--write-resolved", default="")
    doctor.add_argument("--log-dir", default="")
    doctor.add_argument("--check-commands", action="store_true")
    doctor.set_defaults(func=run_doctor)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
