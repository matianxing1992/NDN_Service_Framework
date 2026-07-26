"""Deterministic wire helpers for the Spec 112 segmented-response diagnostic."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from typing import Tuple


REQUEST_SCHEMA = "spec112-segmented-request-v1"
MAX_SEQUENCE_REQUESTS = 10_000
_RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


def positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def run_identity(value: str) -> str:
    if not _RUN_ID.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "run identity must contain 1-128 ASCII letters, digits, '.', '_' or '-'"
        )
    return value


def ndn_name(value: str) -> str:
    if not value.startswith("/") or value == "/" or any(character.isspace() for character in value):
        raise argparse.ArgumentTypeError("must be an absolute non-root NDN name without whitespace")
    return value


def optional_ndn_name(value: str) -> str:
    return "" if value == "" else ndn_name(value)


def encode_request(run_id: str, index: int, size: int) -> bytes:
    run_identity(run_id)
    if index < 0:
        raise ValueError("request index must be non-negative")
    if size < 1:
        raise ValueError("response size must be positive")
    return json.dumps(
        {"index": index, "runId": run_id, "schemaVersion": REQUEST_SCHEMA, "size": size},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")


def decode_request(request: bytes, max_response_bytes: int | None = None) -> Tuple[str, int, int]:
    try:
        value = json.loads(request.decode("ascii"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("request is not canonical diagnostic JSON") from exc
    if not isinstance(value, dict) or value.get("schemaVersion") != REQUEST_SCHEMA:
        raise ValueError("unsupported diagnostic request schema")
    if set(value) != {"index", "runId", "schemaVersion", "size"}:
        raise ValueError("unexpected diagnostic request field")
    run_id = value.get("runId")
    index = value.get("index")
    size = value.get("size")
    try:
        run_identity(run_id)
    except (argparse.ArgumentTypeError, TypeError) as exc:
        raise ValueError("invalid run identity") from exc
    if not isinstance(index, int) or isinstance(index, bool) or index < 0:
        raise ValueError("invalid request index")
    if not isinstance(size, int) or isinstance(size, bool) or size < 1:
        raise ValueError("invalid response size")
    if max_response_bytes is not None and size > max_response_bytes:
        raise ValueError("response size outside configured bound")
    canonical = encode_request(run_id, index, size)
    if canonical != request:
        raise ValueError("request encoding is not canonical")
    return run_id, index, size


def response_payload(size: int, run_id: str, index: int) -> bytes:
    if size < 1:
        raise ValueError("response size must be positive")
    run_identity(run_id)
    output = bytearray()
    block = 0
    while len(output) < size:
        seed = f"SPEC112|{run_id}|{index}|{block}".encode("ascii")
        output.extend(hashlib.sha256(seed).digest())
        block += 1
    return bytes(output[:size])
