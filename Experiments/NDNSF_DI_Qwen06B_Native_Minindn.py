#!/usr/bin/env python3
"""Run a real Qwen3-0.6B CPU native request through a MiniNDN topology.

The Python process owns only topology, identities, files, and child-process
lifecycle.  Request planning, grants, Provider admission, ONNX execution,
streaming, and conversation checkpoints stay in the C++ executables.

    The staged ONNX files are exporter-side contract fixtures.  A canonical ONNX
    source object must be supplied explicitly with ``--canonical-source``; after
    prepare publishes that source, ACK/Selection binds the roles and each Provider
    fetches the selected material and assembles its runner.  The runner never
    treats a pre-exported stage path as a Provider startup artifact.
"""

from __future__ import annotations

import argparse
import atexit
import errno
import fcntl
import hashlib
import json
import math
import os
import re
import shlex
import signal
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives.serialization import (
    Encoding,
    PublicFormat,
    load_pem_public_key,
)
from cryptography.hazmat.backends import default_backend

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Experiments"))
from native_resource_guard import run_guarded, validate_limits
ROLE_PREFIX = "/LLM/Pipeline/Stage/"
SERVICE = "/AI/LLM/Pipeline/QwenNative"
GROUP = "/example/ndnsf-qwen06b/group"
APP_ROOT = "/example/ndnsf-qwen06b"
CONTROLLER = APP_ROOT + "/controller"
AUTHORITY = APP_ROOT + "/authority"
USER = APP_ROOT + "/user"
MODEL_FAMILY = "qwen"
MODEL_URI = "/Model/Qwen3/0.6B"
STATE_PREFIX = "qwen"
TOPIC_PREFIX = "/NDNSF/DI/QWEN"
ARTIFACT_ROOT = "/Artifact/Qwen3-0.6B"
NATIVE_CONVERSATION_JOURNAL_MAX_BYTES = 64 * 1024 * 1024
# ModelPreparationCache reserves one source slot for the ONNX graph and one
# for a separate external initializer, then adds the assembled package and
# parser/configuration scratch.  Keep the formula in one place so a large
# real model cannot be rejected by an undersized experiment-generated limit.
PREPARATION_CACHE_SCRATCH_BYTES = 4 * 1024 * 1024
PUBLICATION_ROOT_MARGIN_BYTES = 16 * 1024 * 1024
LOCAL_ASSEMBLY_DISK_MARGIN_BYTES = 256 * 1024 * 1024
LARGE_DATA_SEGMENT_BYTES = 7000
LARGE_DATA_DEFAULT_IMS_LIMIT = 50000
LARGE_DATA_IMS_MARGIN_SEGMENTS = 8192
LARGE_DATA_IMS_MAX_LIMIT = 1000000
LARGE_MODEL_THRESHOLD_BYTES = 256 * 1024 * 1024
MAX_NEW_TOKENS = 1025
ENCRYPTED_REPOSITORY_LEASE = ".ndnsf-di-encrypted-repo-lease.json"
ENCRYPTED_REPOSITORY_STALE_AFTER_S = 10 * 60
# MiniNDN-only bounded Content Store capacity.  The real Qwen Repo/fetch path
# must not replicate multi-GiB canonical objects in every forwarder; eviction
# remains allowed because the authenticated Repo is the source of truth.
MININDN_NFD_CS_SIZE = 32768
# Keep operator-controlled startup waits finite.  These values are generous
# enough for a slow local MiniNDN launch while preventing a malformed CLI
# value from turning the generated service envelope into an unbounded wait.
MAX_STARTUP_TIMEOUT_S = 600.0
MAX_ROUTING_WAIT_S = 120.0
MAX_QWEN_ROUNDS = 8
MAX_STAGE_COUNT = 32

# Qwen3-0.6B's pinned tokenizer fixture for a minimal non-thinking user turn
# followed by the assistant generation prompt.  The previous fallbacks [1]
# and [0] are valid tensor integers but are not a Qwen chat-template input;
# using them made a real model run measure an invalid prompt rather than
# model generation.  Operators can still override both sequences explicitly.
DEFAULT_QWEN_INPUT_TOKEN_IDS = (
    151644, 872, 198, 9707, 151645, 198, 151644, 77091, 198,
    151667, 271, 151668, 271,
)
DEFAULT_QWEN_DELTA_TOKEN_IDS = (
    198, 151644, 872, 198, 45764, 23811, 1549, 13, 151645, 198,
    151644, 77091, 198, 151667, 271, 151668, 271,
)


def _raise_keyboard_interrupt(signum, _frame) -> None:
    """Route TERM through main's finally block so transient staging is cleaned."""
    raise KeyboardInterrupt(f"received signal {signum}")


# Spec190 narrows only this Qwen multi-turn profile.  Core and other
# profiles retain their existing defaults; the value is passed explicitly in
# the generated native requester contract.
DEFAULT_QWEN_ACK_TIMEOUT_MS = 1000
# A normal operator run uses one bounded disposable workspace.  Durable
# Spec189 evidence must pass --run-root explicitly; that path is never swept
# by this launcher.  The model-source/assembled cache remains outside this
# workspace and is intentionally reusable across runs.
FIXED_WORK_ROOT = Path("/var/tmp/ndnsf-di-spec189")
FIXED_NDNSF_DIRNAME = "ndnsf"
FIXED_REPO_DIRNAME = "repo"
FIXED_WORKSPACE_LOCK = ".active.lock"


def prepare_fixed_workspace(work_root: Path) -> tuple[Path, Path, object]:
    """Lock and reset only the fixed disposable MiniNDN workspace.

    The returned file handle holds an advisory lock for the complete outer
    supervisor lifetime.  Refuse a concurrent run before removing anything.
    Only the named ``ndnsf`` and ``repo`` directories are disposable; the
    cache root and explicitly supplied evidence roots are not in scope.
    """
    work_root = work_root.expanduser().resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    work_root.chmod(0o700)
    lock_path = work_root / FIXED_WORKSPACE_LOCK
    lock = lock_path.open("a+")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (BlockingIOError, OSError) as exc:
        lock.close()
        raise RuntimeError(f"FIXED_WORKSPACE_BUSY:{work_root}") from exc

    try:
        disposable = []
        for name in (FIXED_NDNSF_DIRNAME, FIXED_REPO_DIRNAME):
            path = work_root / name
            if path.is_symlink() or (path.exists() and not path.is_dir()):
                raise RuntimeError(f"FIXED_WORKSPACE_ENTRY_INVALID:{path}")
            if path.exists():
                shutil.rmtree(path)
            path.mkdir(mode=0o700)
            disposable.append(path)
        return disposable[0], disposable[1], lock
    except Exception:
        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        lock.close()
        raise


def release_fixed_workspace(lock: object | None) -> None:
    if lock is None:
        return
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
    finally:
        lock.close()


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def provider_cache_namespace(provider: str) -> str:
    """Return a stable, filesystem-safe namespace for one Provider identity."""
    return "provider-" + hashlib.sha256(provider.encode("utf-8")).hexdigest()


def model_source_cache_identity(model_name: str, model_manifest_digest: str,
                                source_digest: str, graph_digest: str,
                                initializer_digest: str | None,
                                node_mapping_digest: str,
                                tokenizer_digest: str) -> dict:
    """Build the immutable identity for the cross-run canonical source Repo."""
    return {
        "schema": "ndnsf-di-model-source-cache-v1",
        "modelName": model_name,
        "modelManifestDigest": model_manifest_digest,
        "sourceDigest": source_digest,
        "graphDigest": graph_digest,
        "initializerDigest": initializer_digest or "",
        "nodeMappingDigest": node_mapping_digest,
        "tokenizerDigest": tokenizer_digest,
    }


def resolve_model_source_repository(cache_root: Path, identity: dict) -> tuple[Path, str]:
    """Return one system-wide, digest-namespaced canonical source Repo.

    The Repo store is content addressed, so completed source/initializer
    objects can be reused by later runs.  Keep the identity beside the store
    and reject a namespace that was ever associated with another candidate;
    no run-scoped model-sized copy is created here.
    """
    cache_root = cache_root.expanduser().resolve()
    cache_namespace = digest_bytes(
        json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    repository = cache_root / "model-source" / cache_namespace[len("sha256:"):]
    repository.mkdir(parents=True, exist_ok=True)
    repository.chmod(0o700)
    marker = repository / "cache-identity.json"
    encoded = json.dumps(identity, indent=2, sort_keys=True) + "\n"
    if marker.exists():
        try:
            current = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise RuntimeError("MODEL_SOURCE_CACHE_IDENTITY_UNREADABLE") from exc
        if current != identity:
            raise RuntimeError("MODEL_SOURCE_CACHE_IDENTITY_MISMATCH")
    else:
        partial = marker.with_name(marker.name + ".partial")
        partial.write_text(encoded, encoding="utf-8")
        partial.chmod(0o600)
        os.replace(partial, marker)
        marker.chmod(0o600)
    return repository, cache_namespace


def validate_model_source_repository(repository: Path, objects: list[tuple[str, int]]) -> bool:
    """Validate completed content-addressed source objects without loading them."""
    if not repository.is_dir():
        return False
    for expected_digest, expected_size in objects:
        if not expected_digest.startswith("sha256:"):
            raise RuntimeError("MODEL_SOURCE_CACHE_DIGEST_INVALID")
        hex_digest = expected_digest[len("sha256:"):]
        payload = repository / "payloads" / "sha256" / hex_digest[:2] / hex_digest
        if not payload.is_file():
            return False
        if payload.stat().st_size != expected_size:
            raise RuntimeError("MODEL_SOURCE_CACHE_SIZE_MISMATCH")
        if digest_file(payload) != expected_digest:
            raise RuntimeError("MODEL_SOURCE_CACHE_DIGEST_MISMATCH")
    return True


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def digest_text(value: str) -> str:
    return digest_bytes(value.encode("utf-8"))


def load_or_create_persistent_content_key(cache_root: Path, model_family: str) -> tuple[Path, bytes]:
    """Load one fixed authority key so protected assembled cache survives runs.

    The key is deliberately kept outside every run/evidence directory. Its
    non-secret digest is bound into the authority key ID below; otherwise a
    restarted authority could advertise the same cache identity for different
    key material and make an old ciphertext fail only after CACHE_HIT.
    """
    key_directory = cache_root.expanduser().resolve() / "authority-keys"
    key_directory.mkdir(parents=True, exist_ok=True)
    key_directory.chmod(0o700)
    key_path = key_directory / f"{model_family}-content-key-v1.bin"
    if key_path.is_symlink() or (key_path.exists() and not key_path.is_file()):
        raise RuntimeError("CONTENT_KEY_PATH_INVALID")
    if key_path.exists():
        content_key = key_path.read_bytes()
    else:
        content_key = os.urandom(32)
        partial = key_path.with_name(key_path.name + ".partial")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(partial, flags, 0o600)
            with os.fdopen(fd, "wb") as output:
                output.write(content_key)
                output.flush()
                os.fsync(output.fileno())
            os.replace(partial, key_path)
        except FileExistsError as exc:
            raise RuntimeError("CONTENT_KEY_CREATION_RACE") from exc
        finally:
            partial.unlink(missing_ok=True)
    if len(content_key) != 32:
        raise RuntimeError("CONTENT_KEY_SIZE_INVALID")
    key_path.chmod(0o600)
    return key_path, content_key


def preparation_cache_budget(max_source_bytes: int,
                             max_assembled_bytes: int) -> int:
    """Return the requester preparation reservation for the native contract.

    ``ModelPreparationCache`` reserves two bounded source objects (canonical
    graph plus optional external initializer), one assembled object, 256 KiB
    of native scratch, and small JSON/configuration overhead.  The additional
    four MiB margin keeps the generated config independent of incidental JSON
    formatting while remaining finite and model-specific.
    """
    if max_source_bytes <= 0 or max_assembled_bytes <= 0:
        raise ValueError("preparation cache limits must be positive")
    return (2 * max_source_bytes + max_assembled_bytes +
            PREPARATION_CACHE_SCRATCH_BYTES)


def assembled_model_budget(stages: list[dict], source_bytes: int = 0,
                           initializer_bytes: int = 0) -> int:
    """Return the native-contract-aware per-role assembly ceiling.

    Native canonical preparation first retains the complete material set, then
    role assembly may hold selected payloads, the initializer copy, and two
    protobuf/model serialization buffers at once.  Four times the largest
    digest-verified stage is a conservative bound for that selected-role
    working set; the complete source+initializer footprint covers preparation
    manifest generation.  This avoids charging the complete initializer four
    times to every role while still satisfying both native phases.
    """
    if not stages:
        raise ValueError("at least one stage is required for an assembly budget")
    if (not isinstance(source_bytes, int) or isinstance(source_bytes, bool) or
            source_bytes < 0 or not isinstance(initializer_bytes, int) or
            isinstance(initializer_bytes, bool) or initializer_bytes < 0):
        raise ValueError("source and initializer byte sizes must be non-negative integers")
    sizes = []
    for stage in stages:
        size = stage.get("bytes")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            raise ValueError("stage byte sizes must be positive integers")
        sizes.append(size)
    largest_stage = max(sizes)
    selected_role_working_set = 4 * largest_stage
    complete_material_set = source_bytes + initializer_bytes
    return (max(selected_role_working_set, complete_material_set) +
            LOCAL_ASSEMBLY_DISK_MARGIN_BYTES)


def large_data_ims_limit(source_bytes: int, initializer_bytes: int) -> int:
    """Reserve enough bounded IMS entries for this run's canonical objects.

    ``ServiceUser`` keeps the historical 50,000-entry limit unless the
    requester explicitly opts in through ``NDNSF_REQUEST_LARGE_IMS_LIMIT``.
    The limit covers the graph, optional external initializer, envelope/TLV
    overhead, and a finite control-packet margin.  Refuse a value above the
    native hard ceiling instead of silently starting a run that will evict
    early segments and fail much later in ``SegmentFetcher``.
    """
    if source_bytes <= 0 or initializer_bytes < 0:
        raise ValueError("large-data object sizes must be non-negative")
    estimated_bytes = (source_bytes + initializer_bytes +
                       16 * 1024 * 1024)
    segments = ((estimated_bytes + LARGE_DATA_SEGMENT_BYTES - 1) //
                LARGE_DATA_SEGMENT_BYTES)
    required = segments + LARGE_DATA_IMS_MARGIN_SEGMENTS
    limit = max(LARGE_DATA_DEFAULT_IMS_LIMIT, required)
    if limit > LARGE_DATA_IMS_MAX_LIMIT:
        raise ValueError("Qwen canonical objects exceed the native IMS hard ceiling")
    return limit


def qwen_runtime_budgets(source_bytes: int, initializer_bytes: int,
                         rounds: int = 3, stage_count: int = 3,
                         startup_timeout_s: float = 60.0,
                         routing_wait_s: float = 8.0,
                         ack_timeout_ms: int = DEFAULT_QWEN_ACK_TIMEOUT_MS) -> dict[str, int]:
    """Scale preparation/request deadlines only for genuinely large objects.

    The old 30-second bootstrap is suitable for small fixtures but expires
    while the native ONNX identity loader is parsing a real external
    initializer.  Keep ordinary runs bounded and make the large-model budget
    explicit in the generated config and evidence.
    """
    if (source_bytes <= 0 or initializer_bytes < 0 or rounds < 1 or
            rounds > MAX_QWEN_ROUNDS or stage_count < 1 or
            stage_count > MAX_STAGE_COUNT):
        raise ValueError("Qwen runtime budget inputs are outside supported bounds")
    if ack_timeout_ms <= 0:
        raise ValueError("Qwen ACK window must be positive and smaller than the request deadline")
    if (not math.isfinite(startup_timeout_s) or not math.isfinite(routing_wait_s) or
            startup_timeout_s < 0 or routing_wait_s < 0 or
            startup_timeout_s > MAX_STARTUP_TIMEOUT_S or
            routing_wait_s > MAX_ROUTING_WAIT_S):
        raise ValueError(
            "Qwen startup budgets must be finite and within the supported bounds")
    total = source_bytes + initializer_bytes
    if total >= LARGE_MODEL_THRESHOLD_BYTES:
        bootstrap_ms, timeout_ms, no_progress_ms, policy_ms = (
            600000, 900000, 180000, 60000)
        base_service_ms = 1200000
    else:
        bootstrap_ms, timeout_ms, no_progress_ms, policy_ms = (
            60000, 180000, 30000, 5000)
        base_service_ms = 300000
    if ack_timeout_ms >= timeout_ms:
        raise ValueError("Qwen ACK window must be positive and smaller than the request deadline")
    # The turn deadline is written before MiniNDN startup. It must cover the
    # worst-case startup markers, one preparation window, every configured
    # request round, and a bounded commit/cleanup margin. Provider and
    # Authority lifetimes use the same envelope so they cannot exit midway
    # through a later conversation round.
    # The requester is one process now: pay the synchronous Runtime.open/freeze
    # and User.prepare cost once, then reserve one request deadline per turn.
    # Do not retain the old per-process timeout, which could kill a valid later
    # turn even though the overall lifecycle budget already covers all turns.
    preparation_bootstrap_ms = 2 * bootstrap_ms
    process_timeout_s = (preparation_bootstrap_ms + rounds * timeout_ms + 120000 + 999) // 1000
    startup_ms = int((routing_wait_s + startup_timeout_s * (2 + stage_count)) * 1000) + 999
    per_round_ms = timeout_ms
    lifecycle_ms = startup_ms + preparation_bootstrap_ms + rounds * per_round_ms + 120000
    service_ms = max(base_service_ms, lifecycle_ms)
    # Keep the provider's complete post-Selection assembly budget explicit.
    # It is independently wired from the dependency/readiness/control budgets,
    # while this large-model candidate uses the same finite envelope as one
    # native request so material fetch and worker assembly can both complete.
    assembly_timeout_ms = timeout_ms
    return {"bootstrap_ms": bootstrap_ms, "timeout_ms": timeout_ms,
            "assembly_timeout_ms": assembly_timeout_ms,
            "ack_timeout_ms": ack_timeout_ms, "no_progress_ms": no_progress_ms,
            "policy_ms": policy_ms,
            "process_timeout_s": process_timeout_s, "provider_run_ms": service_ms,
            "authority_run_ms": service_ms, "retention_ms": service_ms}


def state_successor_pairs(stages: list[dict], state_prefix: str = "qwen") -> list[tuple[str, str]]:
    """Validate and derive semantic past->present pairs for every stage.

    A positional zip is unsafe: a manifest can keep the same input/output
    sets while swapping a key or value tensor.  Qwen's state contract is
    name-based and every layer must carry one key and one value successor.
    ``state_prefix`` also covers the standard Llama contract used by SmolLM2.
    """
    pairs: list[tuple[str, str]] = []
    previous_end = 0
    for stage_index, stage in enumerate(stages):
        layer_range = stage.get("layerRange")
        if isinstance(layer_range, dict):
            layer_start = int(layer_range.get("start", -1))
            layer_end = int(layer_range.get("endExclusive", -1))
        elif isinstance(layer_range, list) and len(layer_range) == 2:
            layer_start, layer_end = map(int, layer_range)
        else:
            layer_start, layer_end = -1, -1
        if layer_start != previous_end or layer_end <= previous_end:
            raise ValueError("QWEN_LAYER_RANGES_NOT_CONTIGUOUS")
        previous_end = layer_end
        inputs = list(stage.get("cacheInputs", []))
        outputs = list(stage.get("cacheOutputs", []))
        if (not inputs or not outputs or len(inputs) != len(set(inputs)) or
                len(outputs) != len(set(outputs))):
            raise ValueError(f"QWEN_STATE_CONTRACT_INVALID:{stage_index}")

        def indexed(names: list[str], prefix: str) -> dict[str, dict[int, str]]:
            result: dict[str, dict[int, str]] = {"key": {}, "value": {}}
            for name in names:
                parts = name.split(".")
                if len(parts) != 2 or parts[0] not in {prefix + "_key", prefix + "_value"}:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}")
                try:
                    layer = int(parts[1])
                except ValueError as exc:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}") from exc
                family = "key" if parts[0].endswith("_key") else "value"
                if layer < 0 or layer in result[family]:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}")
                result[family][layer] = name
            return result

        past = indexed(inputs, "past")
        present = indexed(outputs, "present")
        expected = set(range(layer_start, previous_end))
        # The exported stages use local layer indexes.  Require both key/value
        # families for exactly the same layer set; order is irrelevant.
        past_keys = set(past["key"])
        past_values = set(past["value"])
        present_keys = set(present["key"])
        present_values = set(present["value"])
        if not (past_keys == past_values == present_keys == present_values == expected):
            raise ValueError(f"QWEN_STATE_LAYER_COVER_INVALID:{stage_index}")
        for layer in sorted(expected):
            pairs.extend([(past["key"][layer], present["key"][layer]),
                          (past["value"][layer], present["value"][layer])])
    return pairs


def qwen_state_successor_pairs(stages: list[dict]) -> list[tuple[str, str]]:
    """Compatibility wrapper for the historical Qwen profile."""
    return state_successor_pairs(stages, "qwen")


def canonical_qwen_state_name(name: str) -> str:
    """Map the profile's semantic KV name to the exported ONNX tensor name.

    The semantic names are stable across the conversation/catalog contract;
    they are not necessarily the names used by one ONNX exporter.  The
    Qwen3 export used by Spec190 names inputs ``past_key_values.N.key`` and
    outputs ``present.N.key`` (and their value counterparts).  Keep this
    conversion in one place so metadata, catalog bindings, and generation
    options cannot drift independently.
    """
    if (re.fullmatch(r"past_key_values\.\d+\.(key|value)", name) or
            re.fullmatch(r"present\.\d+\.(key|value)", name)):
        return name
    match = re.fullmatch(r"past_(key|value)\.(\d+)", name)
    if match:
        return f"past_key_values.{match.group(2)}.{match.group(1)}"
    match = re.fullmatch(r"present_(key|value)\.(\d+)", name)
    if match:
        return f"present.{match.group(2)}.{match.group(1)}"
    raise ValueError(f"QWEN_CANONICAL_STATE_NAME_INVALID:{name}")


def canonical_qwen_state_successor_pairs(
    stages: list[dict[str, Any]],
) -> list[tuple[str, str]]:
    """Return source-bound Qwen KV successor pairs in semantic order."""
    return [
        (canonical_qwen_state_name(input_name),
         canonical_qwen_state_name(output_name))
        for input_name, output_name in qwen_state_successor_pairs(stages)
    ]


def materialize_input(source: Path, destination: Path, expected_digest: str | None,
                      label: str, free_margin: int = LOCAL_ASSEMBLY_DISK_MARGIN_BYTES) -> dict:
    """Create a run-local immutable view without duplicating large artifacts.

    Read-only inputs on the same filesystem are hard-linked, so a local run
    does not need another 1.5 GiB initializer copy.  A cross-device fallback
    copies through a temporary file only after an explicit free-space check.
    The expected digest and stable inode tuple close the prepare-to-run TOCTOU
    boundary; writable sources are rejected because a hard link would share
    their mutable inode.
    """
    source = source.expanduser().resolve()
    destination = destination.expanduser().resolve()
    stat_before = source.stat()
    if not source.is_file() or stat_before.st_size == 0:
        raise RuntimeError(f"{label}_SOURCE_INVALID")
    if stat_before.st_mode & 0o222:
        raise RuntimeError(f"{label}_SOURCE_NOT_IMMUTABLE")
    actual = digest_file(source)
    if expected_digest and actual != expected_digest:
        raise RuntimeError(f"{label}_DIGEST_MISMATCH")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination_existed = destination.exists()
    method = "hardlink"
    try:
        try:
            os.link(source, destination)
        except OSError as exc:
            if exc.errno != errno.EXDEV:
                raise
            usage = shutil.disk_usage(destination.parent)
            required = stat_before.st_size + free_margin
            if usage.free < required:
                raise RuntimeError(f"{label}_DISK_SPACE_INSUFFICIENT") from exc
            method = "copy"
            temporary = destination.with_name(destination.name + ".partial")
            try:
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
                flags |= getattr(os, "O_NOFOLLOW", 0)
                fd = os.open(temporary, flags, 0o600)
                with os.fdopen(fd, "wb") as dst, source.open("rb") as src:
                    while True:
                        chunk = src.read(8 * 1024 * 1024)
                        if not chunk:
                            break
                        dst.write(chunk)
                    dst.flush()
                    os.fsync(dst.fileno())
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
        stat_after = source.stat()
        if ((stat_after.st_dev, stat_after.st_ino, stat_after.st_size,
             stat_after.st_mtime_ns) !=
                (stat_before.st_dev, stat_before.st_ino, stat_before.st_size,
                 stat_before.st_mtime_ns)):
            raise RuntimeError(f"{label}_SOURCE_CHANGED")
        destination_digest = digest_file(destination)
        if expected_digest and destination_digest != expected_digest:
            raise RuntimeError(f"{label}_MATERIALIZED_DIGEST_MISMATCH")
        return {"path": str(destination), "sha256": destination_digest, "method": method,
                "bytes": stat_before.st_size}
    except Exception:
        if not destination_existed:
            destination.unlink(missing_ok=True)
        raise


def resolve_encrypted_repository_path(run_root: Path, requested: Path | None) -> Path:
    """Resolve the run-scoped encrypted Repo directory without changing the default.

    The host guard samples the filesystem containing ``run_root``.  A caller
    may therefore place only the large, regenerated ciphertext Repo on a
    separately validated temporary filesystem; the run root still owns all
    logs, config, identities, and durable evidence.  The external directory
    must be new or empty so one run cannot consume another runs ciphertext.
    """
    default = (run_root / "requester/encrypted-repo").resolve()
    if requested is None:
        return default
    path = requested.expanduser().resolve()
    try:
        path.relative_to(run_root.resolve())
    except ValueError:
        pass
    else:
        raise RuntimeError("ENCRYPTED_REPOSITORY_PATH_MUST_BE_OUTSIDE_RUN_ROOT")
    lease = path.parent / (path.name + ENCRYPTED_REPOSITORY_LEASE)
    if path.exists():
        if not path.is_dir():
            raise RuntimeError("ENCRYPTED_REPOSITORY_PATH_NOT_EMPTY")
        if any(path.iterdir()):
            try:
                metadata = json.loads(lease.read_text(encoding="utf-8"))
                owner_pid = int(metadata["pid"])
                owner_root = Path(metadata["runRoot"]).resolve()
                lease_age = time.time() - lease.stat().st_mtime
                try:
                    os.kill(owner_pid, 0)
                    owner_live = True
                except OSError as error:
                    owner_live = error.errno == errno.EPERM
                if (metadata.get("schema") != "ndnsf-di-encrypted-repo-lease-v1" or
                        owner_root != run_root.resolve() or owner_live or
                        lease_age < ENCRYPTED_REPOSITORY_STALE_AFTER_S):
                    raise RuntimeError("ENCRYPTED_REPOSITORY_PATH_BUSY")
                cleanup_encrypted_repository(path, run_root, require_lease=True)
            except (OSError, ValueError, KeyError, json.JSONDecodeError):
                raise RuntimeError("ENCRYPTED_REPOSITORY_PATH_NOT_EMPTY")
        elif lease.exists():
            lease.unlink()
    else:
        path.mkdir(parents=True, exist_ok=False)
    path.chmod(0o700)
    if requested is not None:
        lease.write_text(json.dumps({
            "schema": "ndnsf-di-encrypted-repo-lease-v1",
            "pid": os.getpid(),
            "runRoot": str(run_root.resolve()),
        }, sort_keys=True) + "\n", encoding="utf-8")
        lease.chmod(0o600)
    return path


def cleanup_encrypted_repository(path: Path, run_root: Path,
                                 require_lease: bool = False) -> bool:
    """Remove only this run's encrypted Repo payloads, preserving evidence.

    ``run_root`` itself is never removed.  An external directory is eligible
    only when its sibling lease belongs to this run; this prevents a failed
    launcher from cleaning another run's ciphertext or the system-wide source
    cache.  The operation is idempotent so an atexit callback and ``finally``
    can safely cover the same failure.
    """
    path = path.expanduser().resolve()
    run_root = run_root.expanduser().resolve()
    default = (run_root / "requester/encrypted-repo").resolve()
    if path == default:
        eligible = True
    else:
        lease = path.parent / (path.name + ENCRYPTED_REPOSITORY_LEASE)
        try:
            metadata = json.loads(lease.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            return False
        eligible = (metadata.get("schema") == "ndnsf-di-encrypted-repo-lease-v1" and
                    Path(metadata.get("runRoot", "")).resolve() == run_root and
                    int(metadata.get("pid", -1)) == os.getpid())
        if require_lease and not eligible:
            return False
    if not eligible or not path.is_dir():
        return False
    removed = False
    for child in list(path.iterdir()):
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink(missing_ok=True)
        removed = True
    if path != default:
        path.rmdir()
        lease.unlink(missing_ok=True)
    return removed or not path.exists()


def require_file_digest(path: Path, expected_digest: str | None, label: str) -> str:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise SystemExit(f"{label}_MISSING:{path}")
    if not expected_digest:
        raise SystemExit(f"{label}_DIGEST_REQUIRED")
    actual = digest_file(path)
    if actual != expected_digest:
        raise SystemExit(f"{label}_DIGEST_MISMATCH")
    return actual


def canonical_source_graph_digest(path: Path) -> str:
    """Derive the source graph identity with the maintained ONNX oracle.

    The Qwen planner graph is a semantic, synthetic graph used for layer
    placement.  The catalog's canonical source identity must instead describe
    the actual ONNX bytes supplied to the native preparer.  Keeping this
    calculation in the maintained ONNX identity helper also avoids silently
    reusing the planner digest for a different identity domain.
    """
    try:
        from ndnsf_distributed_inference.adapters.onnx.graph import (  # type: ignore
            canonical_onnx_identity,
        )
    except ImportError as exc:
        raise RuntimeError(
            "Qwen canonical source identity requires the installed NDNSF ONNX adapter"
        ) from exc
    return canonical_onnx_identity(path).graph_digest


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def qwen_graph_digest(model_name: str, revision: str,
                      precision: str, ranges: list[list[int]]) -> str:
    layers = int(ranges[-1][1])
    nodes = ["embedding"] + [
        f"layer-{index:02d}" for index in range(layers)
    ] + ["final-norm-head"]
    edges = []
    for index in range(1, len(nodes)):
        if index == 1:
            edge = "hidden-embedding-to-layer-00"
        elif index == len(nodes) - 1:
            edge = f"hidden-layer-{layers - 1}-to-final"
        else:
            edge = f"hidden-layer-{index - 2}-to-{index - 1}"
        edges.append(edge)
    return digest_bytes(canonical_bytes({
        "model": model_name,
        "revision": revision,
        "precision": precision,
        "decode_mode": "single-token-autoregressive",
        "modality": "text-only",
        "mtp_enabled": False,
        "thinking_mode": "disabled",
        "layer_ranges": ranges,
        "nodes": nodes,
        "edges": edges,
        "legal_cuts": edges,
    }))


def run_host(command: list[str], *, check: bool = True) -> None:
    subprocess.run(command, cwd=ROOT, check=check,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_external_key(directory: Path, stem: str) -> tuple[Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    private = directory / f"{stem}-private.pem"
    public = directory / f"{stem}-public.pem"
    run_host(["/usr/bin/openssl", "genpkey", "-algorithm", "ED25519",
              "-out", str(private)])
    run_host(["/usr/bin/openssl", "pkey", "-in", str(private), "-pubout",
              "-out", str(public)])
    private.chmod(0o600)
    public.chmod(0o644)
    return private, public


def public_key_digest(path: Path) -> str:
    key = load_pem_public_key(path.read_bytes(), backend=default_backend())
    raw = key.public_bytes(Encoding.Raw, PublicFormat.Raw)
    return digest_bytes(raw)


def parse_csv_ints(value: str) -> list[int]:
    try:
        result = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise ValueError(f"invalid integer list: {value}") from exc
    if not result:
        raise ValueError("integer list must not be empty")
    if any(item < 0 for item in result):
        raise ValueError("token IDs must be non-negative")
    return result


def tensor_bundle(token_ids: list[int]) -> bytes:
    payload = b"".join(struct.pack("<q", value) for value in token_ids)
    return (b"NDITB001" + struct.pack("<I", 1) + struct.pack("<I", 9)
            + b"input_ids" + struct.pack("<I", 3) + struct.pack("<I", 2)
            + struct.pack("<q", 1) + struct.pack("<q", len(token_ids))
            + struct.pack("<Q", len(payload))
            + payload)


def load_stage_manifest(path: Path, stage_root: Path | None):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    schema = str(manifest.get("schema", ""))
    family = str(manifest.get("modelFamily", ""))
    if family not in {"qwen", "llama"}:
        raise ValueError("stage manifest requires an explicit modelFamily")
    if schema != f"ndnsf-di-{family}-onnx-service-manifest-v1":
        raise ValueError("stage manifest family/schema mismatch")
    stages = manifest.get("stages")
    if (not isinstance(stages, list) or len(stages) < 2 or
            len(stages) > MAX_STAGE_COUNT):
        raise ValueError(
            f"Qwen stage manifest requires 2..{MAX_STAGE_COUNT} stages")
    resolved = []
    for stage in stages:
        role = str(stage.get("role", ""))
        expected = f"{ROLE_PREFIX}{len(resolved)}"
        if role != expected:
            raise ValueError(f"stage role must be {expected}, got {role}")
        raw = Path(str(stage.get("path", ""))).expanduser()
        candidates = [raw]
        if not raw.is_absolute():
            candidates.insert(0, path.parent / raw)
            if stage_root:
                candidates.insert(0, stage_root / raw.name)
        artifact = next((item.resolve() for item in candidates if item.is_file()), None)
        if artifact is None:
            raise ValueError(f"stage artifact is missing: {raw}")
        actual = digest_file(artifact)
        expected_digest = str(stage.get("sha256", ""))
        if expected_digest.startswith("sha256:"):
            expected_digest = expected_digest[7:]
        if actual[7:] != expected_digest:
            raise ValueError(f"stage digest mismatch: {artifact}")
        resolved.append({**stage, "path": str(artifact), "sha256": actual,
                         "bytes": artifact.stat().st_size})
    return manifest, resolved


def manifest_eos_token_ids(model_manifest: dict) -> list[int]:
    """Require the prepared tokenizer/config to define the stop contract."""
    values = model_manifest.get("eosTokenIds")
    if isinstance(values, int):
        values = [values]
    if not isinstance(values, (list, tuple)) or not values:
        raise ValueError("MODEL_EOS_TOKEN_IDS_REQUIRED")
    try:
        result = [int(value) for value in values]
    except (TypeError, ValueError) as exc:
        raise ValueError("MODEL_EOS_TOKEN_IDS_INVALID") from exc
    if any(value < 0 for value in result):
        raise ValueError("MODEL_EOS_TOKEN_IDS_INVALID")
    return result


def stage_plan_and_manifest(output: Path, model_manifest: dict, stages: list[dict],
                            max_tokens: int, tokenizer_digest: str,
                            resident_session: bool = False):
    raw_quantization = str(model_manifest.get("quantization", "none")).lower()
    quantization = "weight_only_int8" if raw_quantization == "int8" else raw_quantization
    eos_token_ids = manifest_eos_token_ids(model_manifest)
    state_pairs = canonical_qwen_state_successor_pairs(stages)
    roles = [str(stage["role"]) for stage in stages]
    dependencies = []
    for index in range(len(roles) - 1):
        dependencies.append({
            "producers": [roles[index]], "consumers": [roles[index + 1]],
            "keyScope": f"pipeline-stage-{index}-to-{index + 1}",
            "topicPrefix": TOPIC_PREFIX,
            "objectNameTemplate": (
                f"{{producerProvider}}{TOPIC_PREFIX}/{{sessionId}}/"
                "{producerRole}/{consumerRole}/{sequence}"),
            "expectedSegments": 0, "expectedBytes": 0, "required": True,
            "segmentNaming": {"mode": "ndn-segment-component",
                               "staticSegmentCount": 0, "dynamicFallback": True},
            "tensors": ["hidden_states", "attention_mask", "position_ids"],
        })
    artifacts = []
    for index, stage in enumerate(stages):
        cache_inputs = [canonical_qwen_state_name(name)
                        for name in stage.get("cacheInputs", [])]
        cache_outputs = [canonical_qwen_state_name(name)
                         for name in stage.get("cacheOutputs", [])]
        input_names = [canonical_qwen_state_name(name)
                       if name.startswith(("past_", "present_")) else name
                       for name in stage["inputNames"]]
        output_names = [canonical_qwen_state_name(name)
                        if name.startswith(("past_", "present_")) else name
                        for name in stage["outputNames"]]
        if index > 0 and "hidden_states" in input_names:
            input_names[input_names.index("hidden_states")] = (
                f"{STATE_PREFIX}_s{index - 1}_hidden_states_out")
        if index < len(stages) - 1 and "hidden_states_out" in output_names:
            output_names[output_names.index("hidden_states_out")] = (
                f"{STATE_PREFIX}_s{index}_hidden_states_out")
        metadata = {
            "inputNames": ",".join(input_names),
            "outputNames": ",".join(output_names),
            "forceOutputBundle": "true",
            "executionProvider": "cpu",
            "allowCpuFallback": "false",
            "deviceId": "cpu0",
            "dtype": str(model_manifest.get("dtype", "float32")),
            "quantization": quantization,
            "passthroughTensors": "attention_mask,position_ids",
            "kvTensorMap": ",".join(
                f"{a}={b}" for a, b in state_pairs
                if a in cache_inputs and b in cache_outputs),
            "kvOutputTensors": ",".join(cache_outputs),
            "kvOutputScope": "kv-state",
            "positionInputPolicy": f"{MODEL_FAMILY}-causal-position-v1",
            "attentionMaskInputName": "attention_mask",
            "positionIdsInputName": "position_ids",
            "outputBundleScope": "final-response" if index == len(stages) - 1 else f"pipeline-stage-{index}-to-{index + 1}",
            "streamingGeneration": "true",
            "statefulModel": "true",
            "stateInputNames": ",".join(cache_inputs),
            "stateOutputNames": ",".join(cache_outputs),
            "maxGeneratedTokens": str(max_tokens),
            "eosTokenIds": ",".join(str(item) for item in eos_token_ids),
            "tokenizerDigest": tokenizer_digest,
        }
        if index > 0:
            for name in (f"{STATE_PREFIX}_s{index - 1}_hidden_states_out",
                         "attention_mask", "position_ids"):
                metadata[f"inputScope.{name}"] = f"pipeline-stage-{index - 1}-to-{index}"
        artifacts.append({
            "role": stage["role"], "path": stage["path"],
            "artifact": f"{ARTIFACT_ROOT}/Stage/{index}",
            "filename": Path(stage["path"]).name, "kind": "model",
            # This digest identifies the exporter-side stage contract only.
            # Provider materialization is bound to the authenticated role
            # projection and assembled from the canonical source after
            # Selection; it must never use this local path as a runner input.
            "exportArtifactDigest": stage["sha256"],
            "materialization": "post-selection-canonical-assembly",
            "backend": "onnxruntime", "metadata": metadata,
        })
        if resident_session:
            artifacts[-1]["metadata"]["residentSession"] = "true"
    model_name = str(model_manifest["model"])
    revision = str(model_manifest.get("modelRevision", ""))
    model_uri = MODEL_URI
    plan = {"version": 2, "services": [{
        "schemaVersion": 2, "service": SERVICE, "model": model_uri,
        "modelRepository": model_name, "modelRevision": revision,
        "dtype": model_manifest.get("dtype", "float32"), "modelFamily": MODEL_FAMILY,
        "modelFormat": "onnx", "plannerKind": f"native-{MODEL_FAMILY}-layer",
        "runtimeBackend": "onnxruntime", "executionPolicy": "DATA_DRIVEN_V2",
        "quantization": quantization,
        "roles": roles, "dependencies": dependencies,
    }]}
    service_manifest = {"services": [{
        "name": SERVICE, "model": model_uri, "modelRepository": model_name,
        "modelRevision": revision, "dtype": model_manifest.get("dtype", "float32"),
        "roles": roles, "dependencies": dependencies, "artifacts": artifacts,
        "modelFamily": MODEL_FAMILY, "modelFormat": "onnx",
        "plannerKind": f"native-{MODEL_FAMILY}-layer", "runtimeBackend": "onnxruntime",
        "quantization": quantization,
    }]}
    plan_path = output / f"native-{MODEL_FAMILY}-execution-plan.json"
    manifest_path = output / f"native-{MODEL_FAMILY}-service-manifest.json"
    plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n")
    manifest_path.write_text(json.dumps(service_manifest, indent=2, sort_keys=True) + "\n")
    return plan_path, manifest_path


def policy_text(provider_names: list[str]) -> str:
    entries = "".join(
        " provider-policy\n {\n  for %s\n  allow { %s\n  %s/ROLE/LLM/Pipeline/Stage/%d }\n }\n"
        % (name, SERVICE, SERVICE, index)
        for index, name in enumerate(provider_names))
    return (f"name {CONTROLLER}/NDNSF/ControllerPolicy/v1\n\n"
            "provider-policies\n{\n provider-policy\n {\n  for " + AUTHORITY
            + "\n  allow { /HELLO }\n }\n" + entries + "}\n\n"
            "user-policies\n{\n user-policy\n {\n  for " + USER
            + "\n  allow { " + SERVICE + "\n  /HELLO }\n }\n}\n")


def env_for(home: Path, node: str) -> dict[str, str]:
    env = {"HOME": str(home), "NDN_CLIENT_CONF": str(home / ".ndn/client.conf"),
           "NDN_CLIENT_PIB": "pib-sqlite3:" + str(home / "pib"),
           "NDN_CLIENT_TPM": "tpm-file:" + str(home / "tpm"),
           "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{node}.sock",
           "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"}
    # Required RuntimeEvidence stage/terminal records use WARN. Without a
    # default, the failure monitor cannot see native request failures.
    env["NDN_LOG"] = os.environ.get("NDNSF_NDN_LOG") or "*=WARN"
    stream_diagnostic = os.environ.get("NDNSF_STREAM_GRANT_DIAGNOSTIC", "")
    if stream_diagnostic:
        env["NDNSF_STREAM_GRANT_DIAGNOSTIC"] = stream_diagnostic
    protected_dataflow_diagnostic = os.environ.get(
        "NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC", "")
    if protected_dataflow_diagnostic:
        env["NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC"] = protected_dataflow_diagnostic
    runtime_timing = os.environ.get("NDNSF_DI_RUNTIME_TIMING", "")
    if runtime_timing:
        env["NDNSF_DI_RUNTIME_TIMING"] = runtime_timing
    dependency_trace = os.environ.get("NDNSF_DI_DEPENDENCY_OBJECT_TRACE", "")
    if dependency_trace:
        env["NDNSF_DI_DEPENDENCY_OBJECT_TRACE"] = dependency_trace
    # Keep provider-side large-data/assignment diagnostics explicit and
    # run-scoped.  These are observation controls only; they must reach every
    # MiniNDN process through the maintained launcher instead of relying on a
    # host environment leak.
    for key in ("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE",
                "NDNSF_COLLAB_LARGE_FETCH_TIMING",
                "NDNSF_SELECTION_STATUS_TRACE",
                "SPEC175_TRACE",
                "NDNSF_PHASE_TIMING",
                "NDNSF_TIMELINE_TRACE",
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE",
                "NDNSF_STREAM_PACKET_TIMELINE_TRACE"):
        value = os.environ.get(key, "")
        if value:
            env[key] = value
    return env


def env_command(env: dict[str, str], command: str) -> str:
    return "env " + " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items()) + " " + command


def mini_ndn_node_app_plan(topology_nodes: set[str], controller_node: str,
                           user_node: str, stage_nodes: list[str],
                           provider_names: list[str]) -> dict[str, list[str]]:
    """Return and validate the explicit MiniNDN node-to-application plan."""
    role_nodes = [controller_node, user_node, *stage_nodes]
    if len(role_nodes) != len(set(role_nodes)):
        raise RuntimeError("MiniNDN role nodes must be distinct")
    missing = sorted(set(role_nodes) - topology_nodes)
    if missing:
        raise RuntimeError("topology is missing role nodes: " + ",".join(missing))
    if len(stage_nodes) != len(provider_names):
        raise RuntimeError("provider application plan does not match stage count")

    plan = {node: ["Nfd"] for node in sorted(topology_nodes)}
    plan[controller_node].extend(["App_ServiceController", "DI_NativeArtifactAuthority"])
    plan[user_node].append("DI_NativeRequester")
    for node, provider in zip(stage_nodes, provider_names):
        plan[node].append(f"di-native-provider[{provider}]")
    return plan


def wait_for_marker(process, log: Path, markers: tuple[str, ...], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace") if log.exists() else ""
        for marker in markers:
            if marker in text:
                return marker
        if process.poll() is not None:
            raise RuntimeError(f"process exited {process.returncode}: {log}")
        time.sleep(0.2)
    raise RuntimeError(f"timeout waiting for {markers}: {log}")


def _read_log_tail(log: Path, max_bytes: int = 256 * 1024) -> str:
    """Read only the bounded tail needed for a terminal marker scan."""
    if not log.exists():
        return ""
    with log.open("rb") as stream:
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        stream.seek(max(0, size - max_bytes), os.SEEK_SET)
        return stream.read(max_bytes).decode(errors="replace")


def provider_terminal_failure(provider_logs: list[Path]) -> str | None:
    """Return the first request-time Provider failure observed in a log."""
    markers = (
        "NDNSF_DI_PROVIDER_STAGE stage=TERMINAL status=failed",
        "NDNSF_DI_NATIVE_FAILURE ",
    )
    for log in provider_logs:
        if not log.exists():
            continue
        text = _read_log_tail(log)
        for line in reversed(text.splitlines()):
            if any(marker in line for marker in markers):
                return f"{log}: {line.strip()}"
    return None


def purge_minindn_large_data_cache(nodes, run_root: Path,
                                   trigger: str = "all-conversation-turns-complete") -> None:
    """Evict model/preparation data only after the complete request chain.

    The authenticated Repo remains the source of truth.  This only removes
    transient forwarder copies after all conversation turns and their Provider
    finalization have completed; execution data and certificates use different
    prefixes.  Purging at ``RUNNER_READY`` would erase material still needed
    by a later turn before the durable protected-cache task is complete.
    """
    prefixes = (MODEL_URI, USER + "/NDNSF/LARGE-DATA")
    records = []
    for node in nodes:
        for prefix in prefixes:
            command = (
                "timeout --signal=TERM 15s nfdc cs erase "
                f"{shlex.quote(prefix)} >/dev/null 2>&1; rc=$?; "
                "printf 'NDNSF_CS_ERASE_RC=%s\\n' \"$rc\""
            )
            output = node.cmd(command)
            marker = "NDNSF_CS_ERASE_RC="
            values = [line.rsplit("=", 1)[-1].strip()
                      for line in output.splitlines() if marker in line]
            if values != ["0"]:
                raise RuntimeError(
                    f"MiniNDN NFD CS purge failed on {node.name} for {prefix}: {output!r}")
            records.append({"node": node.name, "prefix": prefix, "returncode": 0})
    (run_root / "ndn-cache-purge.json").write_text(
        json.dumps({"schema": "ndnsf-minindn-large-data-cache-purge-v1",
                    "trigger": trigger,
                    "records": records}, indent=2, sort_keys=True) + "\n")
    print("NDNSF_MININDN_LARGE_DATA_CACHE_PURGED " +
          json.dumps({"nodes": len(nodes), "prefixes": list(prefixes)}, sort_keys=True),
          flush=True)


def wait_for_native_round(process, request_log: Path,
                          provider_logs: list[Path], timeout: float) -> str:
    """Wait for a request while aborting promptly after Provider failure.

    A requester can remain alive while the selected Provider has already
    emitted a terminal failure. Polling only ``wait()`` would retain every
    MiniNDN process until the large-model timeout and delay finally cleanup.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # A success marker precedes process exit. Reap before accepting the
        # round, and read the log after poll so an exited process's final
        # flushed markers cannot be missed by an earlier read.
        returncode = process.poll()
        text = request_log.read_text(errors="replace") if request_log.exists() else ""
        failure = provider_terminal_failure(provider_logs)
        if failure:
            raise RuntimeError(
                f"native request aborted after Provider terminal failure: {failure}")
        if returncode is not None:
            return text
        time.sleep(0.2)
    raise RuntimeError(f"timeout waiting for native request: {request_log}")


def split_native_turn_logs(text: str, rounds: int) -> list[str]:
    """Split one native requester log into immutable per-turn oracle views."""
    lines = text.splitlines(keepends=True)
    starts = []
    for index, line in enumerate(lines):
        marker = "NATIVE_CONVERSATION_TURN_START index="
        if marker not in line:
            continue
        prefix, value = line.split(marker, 1)
        try:
            turn = int(value.split()[0])
        except (ValueError, IndexError) as exc:
            raise RuntimeError("native requester turn-start marker is malformed") from exc
        if prefix or turn != len(starts):
            raise RuntimeError("native requester turn-start markers are not ordered")
        starts.append(index)
    if len(starts) != rounds:
        raise RuntimeError("native requester turn count does not match configuration")
    preamble = "".join(lines[:starts[0]])
    result = []
    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else len(lines)
        chunk = "".join(lines[start:end])
        # The C++ oracle opens each per-turn file as an independent view. Keep
        # run-scoped declarations (for example cache mode) in every view, not
        # only in the physical first log.
        chunk = preamble + chunk
        if chunk.count("NATIVE_CONVERSATION_TURN_COMPLETE") != 1 or \
                chunk.count("NATIVE_REQUEST_SUCCEEDED") != 1:
            raise RuntimeError("native requester turn is missing one terminal success")
        result.append(chunk)
    return result


def conversation_turn_request_id(text: str) -> str:
    """Return the request ID declared by one native conversation turn."""
    marker = "NATIVE_CONVERSATION_TURN_START "
    for line in text.splitlines():
        if marker not in line:
            continue
        _, _, payload = line.partition(marker)
        for token in payload.split():
            key, equal, value = token.partition("=")
            if equal and key == "request" and value:
                return value
        break
    raise RuntimeError("native conversation turn request marker is missing")


def validate_native_output(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"native output is not JSON: {path}") from exc
    if value.get("schema") != "NDNSF-DI-FINAL-V1":
        raise RuntimeError(f"native output schema is unexpected: {path}")
    token_ids = value.get("tokenIds")
    if not isinstance(token_ids, list) or not token_ids:
        raise RuntimeError(f"native output has no generated token IDs: {path}")
    if any(not isinstance(item, int) for item in token_ids):
        raise RuntimeError(f"native output token IDs are not integers: {path}")
    return {"tokenCount": len(token_ids), "tokenIds": token_ids}


def run_cpp_oracle(binary: Path, run_root: Path, cache_compatibility: bool,
                   timeout: float, require_multi_token: bool = False, rounds: int = 1) -> dict:
    """Keep the explicit cache diagnostic distinct from full-path evidence."""
    arguments = [str(binary)]
    if cache_compatibility:
        arguments.append("--cache-compatibility")
    if require_multi_token:
        arguments.append("--require-multi-token")
    if rounds != 1:
        arguments.extend(["--rounds", str(rounds)])
    arguments.extend(["--run-root", str(run_root)])
    marker = ("SPEC189_CPP_CACHE_DIAGNOSTIC_PASS" if cache_compatibility
              else "SPEC189_CPP_ORACLE_PASS")
    log = run_root / "spec189-cpp-oracle.log"
    with log.open("w") as handle:
        result = subprocess.run(arguments, cwd=str(ROOT), stdout=handle,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=timeout, check=False)
    output = log.read_text(errors="replace")
    if (result.returncode != 0 or
            not any(line.startswith(marker + " ") for line in output.splitlines())):
        raise RuntimeError(f"SPEC189_CPP_ORACLE_FAIL rc={result.returncode}: {log}")
    return {"binary": str(binary), "log": str(log), "returncode": result.returncode,
            "scope": "cache-compatible-execution" if cache_compatibility else "full-path",
            "status": "CACHE_DIAGNOSTIC_PASS" if cache_compatibility else "PASS"}


def run_cpp_revocation_oracle(binary: Path, run_root: Path, timeout: float) -> dict:
    """Assert the expected post-revocation failure in the C++ production oracle."""
    arguments = [str(binary), "--expect-revocation-failure", "--run-root", str(run_root)]
    log = run_root / "spec189-cpp-revocation-oracle.log"
    try:
        with log.open("w") as handle:
            result = subprocess.run(arguments, cwd=str(ROOT), stdout=handle,
                                    stderr=subprocess.STDOUT, text=True,
                                    timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"SPEC189_CPP_REVOCATION_ORACLE_FAIL boundary=timeout log={log}") from error
    output = log.read_text(errors="replace")
    marker = "SPEC189_CPP_REVOCATION_FAIL_CLOSED_PASS "
    if (result.returncode != 0 or
            not any(line.startswith(marker) for line in output.splitlines())):
        raise RuntimeError(f"SPEC189_CPP_REVOCATION_ORACLE_FAIL rc={result.returncode}: {log}")
    return {"binary": str(binary), "log": str(log), "returncode": result.returncode,
            "scope": "t007-resident-revocation", "status": "PASS"}


def continuation_config(first_config: dict, round_index: int, delta_ids: list[int]) -> dict:
    """Wire each requester to its immediate parent; native code owns validation."""
    if not 1 <= round_index < MAX_QWEN_ROUNDS:
        raise ValueError("continuation round must be between 1 and 7")
    cfg = json.loads(json.dumps(first_config))
    cfg["request"]["options_file"] = f"options-{round_index}.json"
    parent = ("conversation-state.json" if round_index == 1
              else f"conversation-state-{round_index - 1}.json")
    cfg["conversation"]["turn"] = {
        "mode": "APPEND_DELTA", "generation_id": f"{round_index:032x}",
        "parent_state_file": parent, "delta_token_ids": list(delta_ids)}
    cfg["conversation"]["checkpoint_output_file"] = f"conversation-state-{round_index}.json"
    return cfg


def continuation_options(first_options: dict, round_index: int) -> dict:
    """Preserve the model-specific KV and position contract across turns."""
    if not 1 <= round_index < MAX_QWEN_ROUNDS:
        raise ValueError("continuation round must be between 1 and 7")
    options = json.loads(json.dumps(first_options))
    options["generationId"] = f"{round_index:032x}"
    options["sampling"]["seed"] = 18406 + round_index
    return options


def snapshot_provider_logs(provider_logs: list[Path]) -> dict:
    """Freeze the byte boundary before launching a round; never reuse old lines."""
    result = {}
    for path in provider_logs:
        with path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            partial = False
            if stat.st_size:
                stream.seek(stat.st_size - 1)
                partial = stream.read(1) != b"\n"
            result[path] = (stat.st_dev, stat.st_ino, stat.st_size, partial)
    return result


def provider_log_delta(offsets: dict) -> dict[Path, str]:
    """Read complete lines appended after a Provider log snapshot."""
    result = {}
    for path, (device, inode, offset, partial) in offsets.items():
        with path.open("rb") as stream:
            stat = os.fstat(stream.fileno())
            if (stat.st_dev, stat.st_ino) != (device, inode) or stat.st_size < offset:
                raise RuntimeError("Provider log replaced/truncated")
            stream.seek(offset)
            raw = stream.read()
        if partial:
            raw = raw.partition(b"\n")[2]
        complete = raw[:raw.rfind(b"\n") + 1] if b"\n" in raw else b""
        result[path] = complete.decode("utf-8", errors="strict")
    return result


def _structured_marker_fields(line: str, marker: str) -> dict[str, str] | None:
    """Parse one whitespace-delimited native evidence marker."""
    _, found, payload = line.partition(marker)
    if not found:
        return None
    fields = {}
    for token in payload.split():
        key, equal, value = token.partition("=")
        if not equal or not key or not value or key in fields:
            return None
        fields[key] = value
    return fields


def phase_timing_summary(text: str, request_id: str | None = None) -> dict:
    """Summarize C++ phase records, optionally for one request only."""
    observations = []
    for line in text.splitlines():
        fields = _structured_marker_fields(line, "NDNSF_PHASE_TIMING")
        if not fields:
            continue
        required = ("requestId", "phase", "steady_us", "timestamp_us")
        if any(name not in fields for name in required):
            continue
        if request_id is not None and fields["requestId"] != request_id:
            continue
        try:
            steady = int(fields["steady_us"])
            wall = int(fields["timestamp_us"])
        except ValueError:
            continue
        observations.append({"requestId": fields["requestId"],
                             "phase": fields["phase"],
                             "steadyUs": steady, "wallUs": wall,
                             "tokenIndex": int(fields["tokenIndex"])
                             if fields.get("tokenIndex", "").isdigit() else None})
    by_request = {}
    for observation in observations:
        entry = by_request.setdefault(observation["requestId"], {
            "phaseTimesUs": {}, "phaseWallUs": {}, "tokenReceiveIntervalsUs": []})
        entry["phaseTimesUs"].setdefault(observation["phase"], []).append(
            observation["steadyUs"])
        entry["phaseWallUs"].setdefault(observation["phase"], []).append(
            observation["wallUs"])
    for entry in by_request.values():
        received = entry["phaseTimesUs"].get("tokenReceived", [])
        entry["tokenReceiveIntervalsUs"] = [right - left for left, right in zip(received, received[1:])]
        submit = entry["phaseTimesUs"].get("submit", [])
        terminal = entry["phaseTimesUs"].get("terminal", [])
        checkpoint = entry["phaseTimesUs"].get("checkpointCommitted", [])
        if submit and received:
            entry["ttftUs"] = received[0] - submit[0]
        if submit and terminal:
            entry["submitToTerminalUs"] = terminal[0] - submit[0]
        if checkpoint and terminal:
            entry["checkpointToTerminalUs"] = terminal[0] - checkpoint[0]
    return {"recordCount": len(observations), "requests": by_request}


def onnx_session_load_markers(text: str) -> list[dict[str, str]]:
    """Return Provider-owned runner load evidence; raw provider logs remain authoritative."""
    markers = []
    for line in text.splitlines():
        fields = _structured_marker_fields(line, "NDNSF_DI_ONNX_SESSION_LOAD")
        if fields:
            markers.append(fields)
    return markers


def manifest_resident_session_values(payload) -> list[str]:
    """Collect resident-session declarations from nested service artifacts."""
    values = []
    if isinstance(payload, dict):
        for key, value in payload.items():
            if key == "residentSession":
                values.append(str(value).lower())
            values.extend(manifest_resident_session_values(value))
    elif isinstance(payload, list):
        for value in payload:
            values.extend(manifest_resident_session_values(value))
    return values


def _round_fields(line: str, marker: str) -> dict | None:
    prefix, found, payload = line.partition(marker + " ")
    if not found:
        if marker in line.split():
            raise RuntimeError("Provider barrier malformed marker line")
        return None
    if marker in prefix or marker in payload:
        raise RuntimeError("Provider barrier conflicting marker line")
    fields = {}
    for token in payload.split():
        key, equal, value = token.partition("=")
        if equal:
            if key in fields:
                raise RuntimeError("Provider barrier malformed/conflicting fields")
            fields[key] = value
    return fields


def wait_for_provider_round_barrier(request_text: str, provider_offsets: dict,
                                    placement: dict, tail_role: str,
                                    deadline: float) -> dict:
    """Observe finalization only; stopped is NOT a behavior/qualification PASS.

    placement maps each log to its expected (provider, role). The caller owns
    requester exit/checkpoint validation and the unchanged C++ oracle.
    """
    if (set(provider_offsets) != set(placement) or not placement or
            len({pair[1] for pair in placement.values()}) != len(placement) or
            sum(pair[1] == tail_role for pair in placement.values()) != 1):
        raise RuntimeError("Provider barrier invalid placement")
    success, committed = [], []
    for line in request_text.splitlines(keepends=True):
        if not line.endswith("\n"):
            continue
        for marker, records in (("NATIVE_REQUEST_SUCCEEDED", success),
                                ("NDNSF_DI_NATIVE_SELECTION_COMMITTED", committed)):
            fields = _round_fields(line, marker)
            if fields is not None:
                records.append(fields)
    if len(success) != 1 or len(committed) != 1:
        raise RuntimeError("Provider barrier missing/conflicting requester identity")
    selected = committed[0]
    identity = {key: selected.get(key, "")
                for key in ("requestId", "planDigest", "attemptEpoch")}
    if (not all(identity.values()) or not identity["attemptEpoch"].isdigit() or
            int(identity["attemptEpoch"]) <= 0 or
            success[0].get("request") != identity["requestId"] or
            success[0].get("plan") != identity["planDigest"]):
        raise RuntimeError("Provider barrier requester/Selection identity mismatch")
    while True:
        logs = {}
        for path, (device, inode, offset, partial) in provider_offsets.items():
            with path.open("rb") as stream:
                stat = os.fstat(stream.fileno())
                if (stat.st_dev, stat.st_ino) != (device, inode) or stat.st_size < offset:
                    raise RuntimeError("Provider barrier log replaced/truncated")
                stream.seek(offset)
                raw = stream.read()
            if partial:
                raw = raw.partition(b"\n")[2]
            logs[path] = raw[:raw.rfind(b"\n") + 1].decode("utf-8", errors="strict").splitlines()
        # Scan every Provider before accepting any completion; failures win.
        for lines in logs.values():
            for line in lines:
                if ("NDNSF_DI_NATIVE_FAILURE " in line or
                        ("NDNSF_DI_PROVIDER_STAGE " in line and
                         "status=failed" in line.split())):
                    raise RuntimeError("Provider barrier terminal failure: " + line)
        observations = {}
        for path, lines in logs.items():
            provider, role = placement[path]
            accepted = False
            completed = None
            terminal = False
            mismatched_identity = False
            for line in lines:
                selection = _round_fields(line, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED")
                stage = _round_fields(line, "NDNSF_DI_PROVIDER_STAGE")
                fields = selection if selection is not None else stage
                if fields is None:
                    continue  # Generic execution-completed markers are insufficient.
                # The byte offset excludes completed earlier turns.  Any
                # identity-bearing line appended after that boundary belongs
                # to this barrier and must fail closed if it names another
                # request; silently treating it as an old turn can hide a
                # cross-turn completion.
                if fields.get("requestId") and fields.get("requestId") != identity["requestId"]:
                    mismatched_identity = True
                    continue
                # Intermediate stages (notably feedback DEPENDENCY_FETCH) may
                # legitimately omit a plan digest. Their semantics belong to
                # the native oracle; failures were already checked globally.
                if selection is None and fields.get("stage") not in ("EXECUTION_COMPLETED", "TERMINAL"):
                    continue
                if (any(fields.get(key) != value for key, value in identity.items()) or
                        not fields.get("provider") or not fields.get("role") or
                        fields.get("provider") != provider or fields.get("role") != role):
                    raise RuntimeError("Provider barrier conflicting Provider identity: " + line)
                if selection is not None:
                    if accepted:
                        raise RuntimeError("Provider barrier duplicate Selection")
                    accepted = True
                    continue
                if not accepted:
                    raise RuntimeError("Provider barrier completion before Selection")
                status = fields.get("status")
                if fields["stage"] == "EXECUTION_COMPLETED":
                    if completed is not None or status not in ("observed", "stopped"):
                        raise RuntimeError("Provider barrier conflicting completion")
                    if role == tail_role and status != "observed":
                        raise RuntimeError("Provider barrier tail stopped")
                    completed = status
                else:
                    if role != tail_role or completed != "observed" or terminal or status != "observed":
                        raise RuntimeError("Provider barrier conflicting/out-of-order terminal")
                    terminal = True
            if mismatched_identity and not accepted:
                raise RuntimeError("Provider barrier conflicting Provider identity")
            if completed is not None and (role != tail_role or terminal):
                observations[role] = {"provider": provider, "completionStatus": completed,
                                      "terminalObserved": terminal}
        if time.monotonic() >= deadline:
            raise RuntimeError("Provider barrier timeout waiting for Provider round finalization")
        if len(observations) == len(placement):
            return {"observation": "finalization-only", **identity, "roles": observations}
        time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))


def main(argv=None, *, _supervised=False) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage-manifest", type=Path, required=True)
    parser.add_argument("--model-family", choices=("qwen", "llama"), default="qwen",
                        help="native planner family; llama is used for SmolLM2")
    parser.add_argument("--model-name", default=None,
                        help="model identity from the prepared manifest")
    parser.add_argument("--stage-manifest-sha256", default=None)
    parser.add_argument("--stage-root", type=Path, default=None)
    parser.add_argument("--canonical-source", type=Path, default=None,
                        help="canonical ONNX graph object for native assembly")
    parser.add_argument("--canonical-source-sha256", default=None,
                        help="expected canonical source digest from the prepared candidate")
    parser.add_argument("--canonical-initializer", type=Path, default=None,
                        help="optional external canonical ONNX initializer object")
    parser.add_argument("--canonical-initializer-sha256", default=None,
                        help="expected external initializer digest from the prepared candidate")
    parser.add_argument("--node-mapping", type=Path, required=True,
                        help="authenticated semantic-to-canonical ONNX node mapping JSON")
    parser.add_argument("--node-mapping-sha256", default=None,
                        help="expected node mapping digest from the prepared candidate")
    parser.add_argument("--topology", type=Path, default=ROOT / "Experiments/Topology/AI_Lab.conf")
    parser.add_argument("--topology-sha256", default=None)
    parser.add_argument("--tokenizer-sha256", default=None)
    parser.add_argument("--stage-nodes", default="ucla,arizona",
                        help="execution nodes in stage order; defaults to the two-provider Qwen profile")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--user-node", default="neu")
    parser.add_argument("--build", type=Path, default=ROOT / "build-spec184-b5-candidate-r4")
    parser.add_argument("--build-receipt-sha256", default=None)
    parser.add_argument("--controller-binary", type=Path, default=ROOT / "build-spec184-b5-candidate/examples/App_ServiceController")
    parser.add_argument("--controller-binary-sha256", default=None)
    parser.add_argument("--authority-binary", type=Path, default=None,
                        help="installed NativeArtifactAuthority executable")
    parser.add_argument("--requester-binary", type=Path, default=None,
                        help="installed NativeRequester executable")
    parser.add_argument("--requester-driver-binary", type=Path, default=None,
                        help="optional C++ parent/pipe driver that launches the real requester")
    parser.add_argument("--provider-binary", type=Path, default=None,
                        help="installed native Provider executable")
    parser.add_argument("--assembly-worker-binary", type=Path, default=None,
                        help="installed native ONNX assembly worker executable")
    parser.add_argument("--authority-binary-sha256", default=None)
    parser.add_argument("--requester-binary-sha256", default=None)
    parser.add_argument("--requester-driver-binary-sha256", default=None)
    parser.add_argument("--provider-binary-sha256", default=None)
    parser.add_argument("--assembly-worker-binary-sha256", default=None)
    parser.add_argument("--oracle-binary", type=Path, default=None,
                        help="Spec190 C++ full-path oracle executable")
    parser.add_argument("--oracle-binary-sha256", default=None)
    parser.add_argument(
        "--fixed-work-root", type=Path, default=FIXED_WORK_ROOT,
        help="disposable fixed workspace; explicit --run-root remains durable evidence")
    parser.add_argument("--run-root", type=Path, default=None)
    parser.add_argument(
        "--artifact-cache-root", type=Path,
        default=Path("/var/tmp/ndnsf-di-native-artifacts"),
        help="stable host cache root; provider subdirectories are derived from the provider identity")
    parser.add_argument(
        "--cache-compatibility-mode", action="store_true",
        help="diagnostic mode: after authenticated Selection, Providers read the verified system-wide source cache and skip Repo material fetch")
    parser.add_argument("--resident-session", action="store_true",
                        help="request the bounded CPU ONNX resident-session cache for this candidate")
    parser.add_argument("--encrypted-repository-path", type=Path, default=None,
                        help="optional empty run-scoped ciphertext Repo directory on a separate filesystem")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--ack-timeout-ms", type=int,
                        default=DEFAULT_QWEN_ACK_TIMEOUT_MS,
                        help="Qwen ACK collection window; default is the Spec190 1000ms profile")
    parser.add_argument("--max-new-tokens", type=int, default=2)
    parser.add_argument("--require-multi-token", action="store_true",
                        help="require native multi-token output ending at EOS or the token budget")
    parser.add_argument(
        "--input-token-ids", default="",
        help="comma-separated Qwen token IDs; defaults to the pinned chat-template fixture")
    parser.add_argument(
        "--delta-token-ids", default="",
        help="comma-separated continuation token IDs; defaults to the pinned chat-template fixture")
    parser.add_argument("--negative-parent", action="store_true")
    parser.add_argument(
        "--revoke-after-first-round", action="store_true",
        help="after one successful resident round, revoke the user and require the continuation to fail closed")
    parser.add_argument("--routing-wait-s", "--nlsr-wait-s", dest="routing_wait_s",
                        type=float, default=8.0,
                        help="static-route settle wait; --nlsr-wait-s is a legacy alias")
    parser.add_argument("--startup-timeout-s", type=float, default=60.0)
    parser.add_argument("--resource-limits-json", default="{}",
                        help="host resource limits as a JSON object; defaults match LocalExperiment")
    parser.add_argument(
        "--direct-start", action="store_true",
        help="start MiniNDN in this process; the caller owns host resource supervision")
    args = parser.parse_args(argv)
    # The launcher owns transient Repo staging and MiniNDN processes.  SIGTERM
    # must follow the same cleanup path as Ctrl-C; the default Python action
    # would exit before the finally block removes only this run's staging.
    signal.signal(signal.SIGTERM, _raise_keyboard_interrupt)
    global SERVICE, GROUP, APP_ROOT, CONTROLLER, AUTHORITY, USER
    global MODEL_FAMILY, MODEL_URI, STATE_PREFIX, TOPIC_PREFIX, ARTIFACT_ROOT
    if args.model_family == "llama":
        smollm_model_uris = {
            "HuggingFaceTB/SmolLM2-135M": "/Model/SmolLM2/135M",
            "HuggingFaceTB/SmolLM2-360M": "/Model/SmolLM2/360M",
        }
        if args.model_name not in smollm_model_uris:
            raise SystemExit(
                "SMOLLM2_MODEL_REQUIRED: use HuggingFaceTB/SmolLM2-135M "
                "or HuggingFaceTB/SmolLM2-360M")
        MODEL_FAMILY = "llama"
        MODEL_URI = smollm_model_uris[args.model_name]
        STATE_PREFIX = "llama"
        TOPIC_PREFIX = "/NDNSF/DI/LLAMA"
        ARTIFACT_ROOT = "/Artifact/SmolLM2"
        SERVICE = "/AI/LLM/Pipeline/SmolLM2Native"
        GROUP = "/example/ndnsf-smollm2/group"
        APP_ROOT = "/example/ndnsf-smollm2"
        CONTROLLER = APP_ROOT + "/controller"
        AUTHORITY = APP_ROOT + "/authority"
        USER = APP_ROOT + "/user"
    limits = validate_limits(json.loads(args.resource_limits_json))
    if not _supervised and args.direct_start:
        # LocalExperiment already owns the single host resource supervisor.
        # Enter the MiniNDN path in this process; do not recursively parse the
        # full launcher command or create another worker/supervisor boundary.
        _supervised = True
    if not _supervised:
        workspace_lock = None
        if args.run_root is None:
            run_root, fixed_repo_root, workspace_lock = prepare_fixed_workspace(
                args.fixed_work_root)
        else:
            run_root = args.run_root.expanduser().resolve()
            fixed_repo_root = None
            run_root.mkdir(parents=True, exist_ok=True)
            run_root.chmod(0o700)
        worker_args = list(sys.argv[1:] if argv is None else argv)
        if args.run_root is None:
            worker_args.extend(["--run-root", str(run_root)])
            if (not args.cache_compatibility_mode and
                    args.encrypted_repository_path is None):
                worker_args.extend(["--encrypted-repository-path",
                                    str(fixed_repo_root / "encrypted-repo")])
        # Internal worker entry has no public bypass switch. The outer process
        # owns resource admission before model hashing/materialization begins.
        worker = [sys.executable, "-c",
                  "import runpy,sys; path=sys.argv[1]; sys.argv=sys.argv[1:]; "
                  "scope=runpy.run_path(path); "
                  "raise SystemExit(scope['main'](_supervised=True))",
                  str(Path(__file__).resolve()), *worker_args]
        try:
            try:
                receipt_fd = os.open(run_root / "supervisor.json",
                                     os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:
                print("NATIVE_HOST_GUARD_STOP " + json.dumps({
                    "boundary": "EVIDENCE_CONFLICT", "cleanup": "NOT_STARTED",
                    "returncode": None, "remainingProcesses": []}, sort_keys=True))
                return 1
            with os.fdopen(receipt_fd, "w", encoding="utf-8") as receipt:
                result = run_guarded(worker, cwd=ROOT, stdout=None,
                                     sample_path=run_root / "resource-samples.jsonl", limits=limits)
                json.dump(result, receipt, sort_keys=True, indent=2)
                receipt.write("\n")
            if result["boundary"] or result["cleanup"] != "PASS":
                print("NATIVE_HOST_GUARD_STOP " + json.dumps(result, sort_keys=True))
                return 1
            return result["returncode"] if result["returncode"] is not None else 1
        finally:
            release_fixed_workspace(workspace_lock)
    if os.geteuid() != 0:
        raise SystemExit("MININDN_REQUIRES_ROOT: run this script with sudo -E")
    if args.rounds < 1 or args.rounds > 8:
        raise SystemExit("--rounds must be between 1 and 8")
    if args.negative_parent and args.rounds < 2:
        raise SystemExit("--negative-parent requires --rounds >= 2")
    if args.revoke_after_first_round and args.rounds != 2:
        raise SystemExit("--revoke-after-first-round requires --rounds 2")
    if args.revoke_after_first_round and args.negative_parent:
        raise SystemExit("--revoke-after-first-round cannot be combined with --negative-parent")
    if args.revoke_after_first_round and not args.resident_session:
        raise SystemExit("--revoke-after-first-round requires --resident-session")
    if args.max_new_tokens < 1 or args.max_new_tokens > MAX_NEW_TOKENS:
        raise SystemExit(
            f"--max-new-tokens must be between 1 and {MAX_NEW_TOKENS}")
    if args.require_multi_token and args.max_new_tokens < 2:
        raise SystemExit("--require-multi-token requires at least two output tokens")
    stage_manifest_path = args.stage_manifest.expanduser().resolve()
    require_file_digest(stage_manifest_path, args.stage_manifest_sha256, "MODEL_STAGE_MANIFEST")
    model_manifest, stages = load_stage_manifest(stage_manifest_path, args.stage_root)
    manifest_family = str(model_manifest.get("modelFamily", ""))
    if manifest_family != MODEL_FAMILY:
        raise SystemExit(
            f"MODEL_FAMILY_MANIFEST_MISMATCH: expected {MODEL_FAMILY}, "
            f"got {manifest_family}")
    eos_token_ids = manifest_eos_token_ids(model_manifest)
    state_successor_pairs(stages, STATE_PREFIX)
    stage_nodes = [item.strip() for item in args.stage_nodes.split(",") if item.strip()]
    if len(stage_nodes) != len(stages) or len(set(stage_nodes)) != len(stage_nodes):
        raise SystemExit("--stage-nodes count must match stage manifest and contain no duplicates")
    if not args.topology.expanduser().is_file():
        raise SystemExit(f"topology is missing: {args.topology}")
    require_file_digest(args.topology, args.topology_sha256, "TOPOLOGY")
    tokenizer_raw = Path(str(model_manifest.get("tokenizer", ""))).expanduser()
    tokenizer_candidates = [tokenizer_raw]
    if not tokenizer_raw.is_absolute():
        tokenizer_candidates.extend([
            stage_manifest_path.parent / tokenizer_raw,
            (args.stage_root.expanduser() / tokenizer_raw)
            if args.stage_root else stage_manifest_path.parent / tokenizer_raw,
        ])
    tokenizer = None
    for candidate in tokenizer_candidates:
        resolved = candidate.resolve()
        if resolved.is_dir():
            resolved = resolved / "tokenizer.json"
        if resolved.is_file():
            tokenizer = resolved
            break
    if tokenizer is None:
        raise SystemExit(f"tokenizer.json is missing: {tokenizer_raw}")
    tokenizer_digest = digest_bytes(tokenizer.read_bytes())
    if args.tokenizer_sha256 and tokenizer_digest != args.tokenizer_sha256:
        raise SystemExit("MODEL_TOKENIZER_DIGEST_MISMATCH")
    build = args.build.expanduser().resolve()
    authority_binary = (args.authority_binary.expanduser().resolve()
                        if args.authority_binary is not None
                        else (build / "examples/DI_NativeArtifactAuthority").resolve())
    requester_binary = (args.requester_binary.expanduser().resolve()
                        if args.requester_binary is not None
                        else (build / "examples/DI_NativeRequester").resolve())
    requester_driver_binary = (args.requester_driver_binary.expanduser().resolve()
                               if args.requester_driver_binary is not None else None)
    provider_binary = (args.provider_binary.expanduser().resolve()
                       if args.provider_binary is not None
                       else (build / "examples/di-native-provider").resolve())
    assembly_worker_binary = (args.assembly_worker_binary.expanduser().resolve()
                              if args.assembly_worker_binary is not None
                              else (build / "DI_NativeOnnxAssemblyWorker").resolve())
    oracle_binary = (args.oracle_binary.expanduser().resolve()
                     if args.oracle_binary is not None
                     else (build / "examples/spec190-multiturn-oracle").resolve())
    receipt = build / "spec180-native-build.json"
    # Verify the complete runtime identity before copying the canonical graph
    # or external initializer into the run directory.  The same checks are
    # repeated immediately before child launch below as a TOCTOU fence, but a
    # failed preflight must not first create a model-sized transient copy.
    require_file_digest(receipt, args.build_receipt_sha256, "BUILD_RECEIPT")
    binary_checks = [
        (args.controller_binary, args.controller_binary_sha256, "CONTROLLER_BINARY"),
        (authority_binary, args.authority_binary_sha256, "AUTHORITY_BINARY"),
        (requester_binary, args.requester_binary_sha256, "REQUESTER_BINARY"),
        (provider_binary, args.provider_binary_sha256, "PROVIDER_BINARY"),
        (assembly_worker_binary, args.assembly_worker_binary_sha256, "ASSEMBLY_WORKER_BINARY"),
        (oracle_binary, args.oracle_binary_sha256, "SPEC190_ORACLE_BINARY"),
    ]
    if requester_driver_binary is not None:
        binary_checks.append((requester_driver_binary, args.requester_driver_binary_sha256,
                              "REQUESTER_DRIVER_BINARY"))
    for path, expected, label in binary_checks:
        require_file_digest(path, expected, label)
    revision = str(model_manifest.get("modelRevision", ""))
    model_name = args.model_name or str(model_manifest.get("model", ""))
    if not model_name:
        raise SystemExit("MODEL_NAME_REQUIRED")
    if MODEL_FAMILY == "llama" and model_manifest.get("model") != model_name:
        raise SystemExit("SMOLLM2_MODEL_MANIFEST_IDENTITY_MISMATCH")
    ranges = []
    for stage in stages:
        layer_range = stage.get("layerRange")
        if isinstance(layer_range, dict):
            ranges.append([int(layer_range["start"]), int(layer_range["endExclusive"])])
        elif isinstance(layer_range, list) and len(layer_range) == 2:
            ranges.append([int(layer_range[0]), int(layer_range[1])])
        else:
            raise ValueError(f"stage layerRange is invalid: {stage.get('role')}")
    precision = str(model_manifest.get("dtype", "float32")).lower()
    if precision not in {"float16", "float32"}:
        raise SystemExit(f"unsupported {MODEL_FAMILY} precision: {precision}")
    if MODEL_FAMILY == "llama" and precision != "float32":
        raise SystemExit("SMOLLM2_CPU_FLOAT32_REQUIRED")
    quantization = str(model_manifest.get("quantization", "none")).lower()
    if quantization not in {"none", "int8"}:
        raise SystemExit(f"unsupported {MODEL_FAMILY} quantization: {quantization}")
    if quantization == "int8" and precision != "float32":
        raise SystemExit("INT8_PUBLIC_CONTRACT_MUST_BE_FLOAT32")
    quantization_subtype = "weight_only_int8" if quantization == "int8" else "none"
    graph_digest = qwen_graph_digest(model_name, revision, precision, ranges)
    adapter = {
        "name": MODEL_FAMILY, "version": "1", "state_digest": digest_text(f"{MODEL_FAMILY}-native-state"),
        "abi": f"{MODEL_FAMILY}-native-onnxruntime-cpu-v1", "model_formats": ["onnx"],
        "tasks": ["text-generation"], "backends": ["onnxruntime"], "precisions": [precision],
        "input_schema_digest": digest_text(f"{MODEL_FAMILY}-input-schema"),
        "options_schema_digest": digest_text(f"{MODEL_FAMILY}-options-schema"),
        "result_schema_digest": digest_text(f"{MODEL_FAMILY}-result-schema"),
        "graph_schema_digest": digest_text(f"{MODEL_FAMILY}-graph-schema"),
        "split_schema_digest": digest_text(f"{MODEL_FAMILY}-split-schema"),
        "state_schema_digest": digest_text(f"{MODEL_FAMILY}-state-schema"),
        "graph_inspectable": True, "splittable": True, "deterministic_analysis": True,
    }
    model_descriptor = {
        "model_name": model_name,
        "content_digest": digest_text(f"{model_name}:{revision}:weights:{quantization}"),
        "semantics_digest": digest_text(f"{model_name}:{revision}:semantics:{quantization}"),
        "graph_digest": graph_digest, "model_format": "onnx", "precision": precision,
        "adapter": adapter, "source_revision": revision,
        **({"quantization_subtype": quantization_subtype}
           if quantization_subtype != "none" else {}),
    }
    adapter_digest = digest_bytes(canonical_bytes(adapter))
    manifest_digest = digest_bytes(stage_manifest_path.read_bytes())
    if args.canonical_source is None:
        raise SystemExit("MODEL_CANONICAL_SOURCE_REQUIRED")
    if not args.canonical_source_sha256:
        raise SystemExit("MODEL_CANONICAL_SOURCE_DIGEST_REQUIRED")
    canonical_source_input = args.canonical_source.expanduser().resolve()
    if not canonical_source_input.is_file():
        raise SystemExit(f"MODEL_CANONICAL_SOURCE_MISSING:{canonical_source_input}")
    if canonical_source_input.stat().st_size == 0:
        raise SystemExit("MODEL_CANONICAL_SOURCE_SIZE_INVALID")
    mapping_input = args.node_mapping.expanduser().resolve()
    if not mapping_input.is_file():
        raise SystemExit(f"MODEL_NODE_MAPPING_MISSING:{mapping_input}")
    if not args.node_mapping_sha256:
        raise SystemExit("MODEL_NODE_MAPPING_DIGEST_REQUIRED")
    mapping_digest = digest_file(mapping_input)
    if args.node_mapping_sha256 and mapping_digest != args.node_mapping_sha256:
        raise SystemExit("MODEL_NODE_MAPPING_DIGEST_MISMATCH")
    mapping_payload = json.loads(mapping_input.read_text(encoding="utf-8"))
    node_mapping = mapping_payload.get("mapping", mapping_payload)
    expected_mapping_keys = {"embedding", "final-norm-head"} | {
        f"layer-{index:02d}" for index in range(ranges[-1][1])}
    if not isinstance(node_mapping, dict) or set(node_mapping) != expected_mapping_keys:
        raise SystemExit("MODEL_NODE_MAPPING_COVER_INVALID")
    if any(not isinstance(indices, list) or not indices or
           any(not isinstance(index, int) or index < 0 for index in indices)
           for indices in node_mapping.values()):
        raise SystemExit("MODEL_NODE_MAPPING_INDEX_INVALID")
    source_node_count = mapping_payload.get("source_nodes") if isinstance(mapping_payload, dict) else None
    if (not isinstance(source_node_count, int) or source_node_count <= 0):
        raise SystemExit("MODEL_NODE_MAPPING_SOURCE_COUNT_INVALID")
    mapped_indices = [index for indices in node_mapping.values() for index in indices]
    if (len(mapped_indices) != source_node_count or
            len(set(mapped_indices)) != source_node_count or
            set(mapped_indices) != set(range(source_node_count))):
        raise SystemExit("MODEL_NODE_MAPPING_INDEX_COVER_INVALID")
    canonical_initializer_input = None
    if args.canonical_initializer is not None:
        if not args.canonical_initializer_sha256:
            raise SystemExit("MODEL_CANONICAL_INITIALIZER_DIGEST_REQUIRED")
        canonical_initializer_input = args.canonical_initializer.expanduser().resolve()
        if not canonical_initializer_input.is_file():
            raise SystemExit(f"MODEL_CANONICAL_INITIALIZER_MISSING:{canonical_initializer_input}")
        if canonical_initializer_input.stat().st_size == 0:
            raise SystemExit("MODEL_CANONICAL_INITIALIZER_SIZE_INVALID")
    if args.run_root is None:
        raise SystemExit("RUN_ROOT_REQUIRED_INTERNAL")
    run_root = args.run_root.expanduser().resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    run_root.chmod(0o700)
    artifact_cache_root = args.artifact_cache_root.expanduser().resolve()
    if artifact_cache_root == run_root or run_root in artifact_cache_root.parents:
        raise SystemExit("ARTIFACT_CACHE_ROOT_MUST_NOT_BE_RUN_SCOPED")
    (run_root / "requester").mkdir()
    source_path = run_root / "requester/canonical-source.onnx"
    mapping_path = run_root / "requester/node-mapping.json"
    initializer_path = (run_root / "requester/canonical-initializer.bin"
                         if canonical_initializer_input is not None else None)
    materialized = []
    try:
        source_materialized = materialize_input(
            canonical_source_input, source_path, args.canonical_source_sha256, "MODEL_CANONICAL")
        materialized.append(source_path)
        mapping_materialized = materialize_input(
            mapping_input, mapping_path, args.node_mapping_sha256, "MODEL_NODE_MAPPING")
        materialized.append(mapping_path)
        if initializer_path is not None:
            initializer_materialized = materialize_input(
                canonical_initializer_input, initializer_path,
                args.canonical_initializer_sha256, "MODEL_CANONICAL_INITIALIZER")
            materialized.append(initializer_path)
    except (OSError, RuntimeError) as exc:
        for path in materialized:
            path.unlink(missing_ok=True)
        raise SystemExit(str(exc)) from exc
    source_digest = source_materialized["sha256"]
    initializer_path = None
    initializer_digest = None
    if canonical_initializer_input is not None:
        initializer_path = run_root / "requester/canonical-initializer.bin"
        initializer_digest = initializer_materialized["sha256"]
    # The maintained ONNX identity loader resolves external data relative to
    # this copied source object, so compute it only after the initializer has
    # been materialized in the same requester directory.
    canonical_source_digest = canonical_source_graph_digest(source_path)
    source_cache_identity = model_source_cache_identity(
        model_name, manifest_digest,
        source_digest, canonical_source_digest, initializer_digest,
        mapping_digest, tokenizer_digest)
    model_source_repository, source_cache_namespace = resolve_model_source_repository(
        artifact_cache_root, source_cache_identity)
    source_cache_objects = [(source_digest, source_path.stat().st_size)]
    if initializer_digest:
        source_cache_objects.append(
            (initializer_digest, initializer_path.stat().st_size))
    source_cache_hit = validate_model_source_repository(
        model_source_repository, source_cache_objects)
    if args.cache_compatibility_mode and not source_cache_hit:
        raise SystemExit("MODEL_SOURCE_CACHE_REQUIRED_FOR_COMPATIBILITY_MODE")
    print("MODEL_SOURCE_CACHE " + json.dumps({
        "namespace": source_cache_namespace,
        "path": str(model_source_repository),
        "hashesVerified": source_cache_hit,
        "objects": len(source_cache_objects),
    }, sort_keys=True), flush=True)
    stage_plan, service_manifest = stage_plan_and_manifest(
        run_root, model_manifest, stages, args.max_new_tokens, tokenizer_digest,
        args.resident_session)
    provider_names = [f"{APP_ROOT}/provider-{index}" for index in range(len(stages))]
    role_map_digest = digest_bytes(canonical_bytes(list(zip(
        [stage["role"] for stage in stages], provider_names))))
    # The preparation cache reservation is conservative: it includes the
    # largest role assembly plus parser scratch, while the provider cache
    # enforces the same per-role assembly ceiling.  Keep this model-specific
    # instead of reserving an 8 GiB generic ceiling for a 0.6B local run.
    max_source_bytes = max(
        canonical_source_input.stat().st_size,
        canonical_initializer_input.stat().st_size
        if canonical_initializer_input is not None else 0,
    ) + 64 * 1024 * 1024
    canonical_source_bytes = canonical_source_input.stat().st_size
    canonical_initializer_bytes = (canonical_initializer_input.stat().st_size
                                   if canonical_initializer_input is not None else 0)
    # This is a per-role output ceiling.  Do not charge the complete external
    # initializer to every Provider's assembled layer: that inflated the
    # preparation reservation to 9 GiB for the 0.6B candidate and encouraged
    # the host to swap.  The canonical source/initializer remain separately
    # bounded by max_source_bytes.
    max_assembled_bytes = assembled_model_budget(
        stages, canonical_source_input.stat().st_size, canonical_initializer_bytes)
    max_prepared_bytes = preparation_cache_budget(
        max_source_bytes, max_assembled_bytes)
    large_data_ims = large_data_ims_limit(
        canonical_source_bytes, canonical_initializer_bytes)
    runtime_budgets = qwen_runtime_budgets(
        canonical_source_bytes, canonical_initializer_bytes,
        rounds=args.rounds, stage_count=len(stages),
        startup_timeout_s=args.startup_timeout_s, routing_wait_s=args.routing_wait_s,
        ack_timeout_ms=args.ack_timeout_ms)
    print("SPEC190_ACK_WINDOW " + json.dumps({
        "ackTimeoutMs": runtime_budgets["ack_timeout_ms"],
        "requestTimeoutMs": runtime_budgets["timeout_ms"],
        "profile": "qwen-multiturn",
    }, sort_keys=True), flush=True)
    # Keep semantic names as the mapping keys consumed by the native planner,
    # but bind each value and generation option to the actual Qwen ONNX name.
    state_inputs = {
        stage["role"]: {
            name: [canonical_qwen_state_name(name)]
            for name in stage.get("cacheInputs", [])
        }
        for stage in stages
    }
    state_outputs = {
        stage["role"]: {
            name: [canonical_qwen_state_name(name)]
            for name in stage.get("cacheOutputs", [])
        }
        for stage in stages
    }
    all_state_inputs = [canonical_qwen_state_name(name)
                        for stage in stages
                        for name in stage.get("cacheInputs", [])]
    all_state_outputs = [canonical_qwen_state_name(name)
                         for stage in stages
                         for name in stage.get("cacheOutputs", [])]
    catalog = {
        "schema": "ndnsf-di-native-request-catalog-v1", "model": model_descriptor,
        "source": {"data_name": f"/catalog/{MODEL_FAMILY}/source", "digest": source_digest,
                    "model_manifest_digest": manifest_digest,
                    "canonical_graph_digest": canonical_source_digest,
                    **({"initializer_digest": initializer_digest}
                       if initializer_digest else {})},
        "recipe": {"artifact_profile_digest": digest_text(
                       f"{MODEL_FAMILY}-profile:{quantization_subtype}"),
                   "assembler_descriptor_digest": digest_text(
                       f"{MODEL_FAMILY}-assembler:{quantization_subtype}"),
                   "backend_abi": "onnxruntime-cpu-v1", "precision": precision,
                   "quantization": quantization_subtype, "layout": "native", "padding": "none",
                   "protection_epoch": "epoch-1", "max_source_bytes": max_source_bytes,
                   "max_assembled_bytes": max_assembled_bytes, "max_nodes": 10000},
        "publication": {"artifact_root": MODEL_URI + "/artifacts",
                         "package_manifest_digest": manifest_digest,
                         # Prepare publishes one topology-independent material
                         # set plus its authenticated root. This budget is
                         # separate from each selected role's assembly limit.
                         "max_publication_bytes": (max_source_bytes +
                                                    max_assembled_bytes +
                                                    PUBLICATION_ROOT_MARGIN_BYTES)},
        "input_format": "OPAQUE", "max_payload_bytes": 4 << 20,
        # Conversation input is an authenticated tensor bundle.  The native
        # catalog adapter must know how to extract input_ids before it can
        # create the first conversation turn; omitting this contract makes a
        # real request fail at the input boundary before ACK/Selection.
        "conversation_input": {"kind": "TENSOR_BUNDLE_TOKEN_IDS",
                                "tensor_name": "input_ids"},
        "splitter": {"kind": MODEL_FAMILY.upper(), "model_family": MODEL_FAMILY, "layer_ranges": ranges,
                     "artifact_digests_by_role": {stage["role"]: stage["sha256"] for stage in stages},
                     "weight_bytes_by_role": {stage["role"]: stage["bytes"] for stage in stages},
                     "roles": [stage["role"] for stage in stages],
                     "tensor_degrees": [1] * len(stages),
                     "input_ingress_role": stages[0]["role"],
                     "result_egress_role": stages[-1]["role"]},
        "node_mapping": node_mapping,
        "state_inputs": state_inputs,
        "state_outputs": state_outputs,
    }
    requester_dir = run_root / "requester"
    catalog_path = requester_dir / "catalog.json"
    catalog_path.write_text(json.dumps(catalog, indent=2, sort_keys=True) + "\n")
    input_ids = (parse_csv_ints(args.input_token_ids)
                 if args.input_token_ids else list(DEFAULT_QWEN_INPUT_TOKEN_IDS))
    (requester_dir / "input.bin").write_bytes(tensor_bundle(input_ids))
    delta_ids = (parse_csv_ints(args.delta_token_ids)
                 if args.delta_token_ids else list(DEFAULT_QWEN_DELTA_TOKEN_IDS))
    (requester_dir / "delta.bin").write_bytes(tensor_bundle(delta_ids))
    policy = run_root / "policy.conf"
    policy.write_text(policy_text(provider_names))
    (run_root / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    authority_dir = run_root / "authority"; authority_dir.mkdir()
    provider_dirs = []
    for index in range(len(stages)):
        path = run_root / f"provider-{index}"; path.mkdir(); provider_dirs.append(path)
    req_private, req_public = make_external_key(requester_dir, "requester")
    auth_private, auth_public = make_external_key(authority_dir, "authority")
    content_key_path, content_key = load_or_create_persistent_content_key(
        artifact_cache_root, MODEL_FAMILY)
    content_key_id = f"{MODEL_FAMILY}-model-key-{digest_bytes(content_key)[len('sha256:'):]}"
    provider_keys = {}
    for index, directory in enumerate(provider_dirs):
        recipient_private, recipient_public = make_external_key(directory, "recipient")
        offer_private, offer_public = make_external_key(directory, "offer")
        provider_keys[index] = (recipient_private, recipient_public, offer_private, offer_public)
    # Build one public certificate set, copy it to each node, then remove
    # private key files for identities that node does not own.  This mirrors
    # a real deployment: validators can resolve peer certificates without
    # turning every MiniNDN host into a shared private-key store.
    nodes = [args.controller_node, args.user_node, *stage_nodes]
    homes = {node: run_root / "homes" / node for node in nodes}
    for node, home in homes.items():
        (home / ".ndn").mkdir(parents=True, exist_ok=True)
        (home / "pib").mkdir(parents=True, exist_ok=True)
        (home / "tpm").mkdir(parents=True, exist_ok=True)
        (home / ".ndn/client.conf").write_text(f"transport=unix:///run/nfd/{node}.sock\n")
    identities = [(args.controller_node, CONTROLLER), (args.controller_node, AUTHORITY),
                  (args.user_node, USER), *zip(stage_nodes, provider_names)]
    bootstrap_home = run_root / "homes/bootstrap"
    (bootstrap_home / ".ndn").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "pib").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "tpm").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / ".ndn/client.conf").write_text("transport=unix:///run/nfd/bootstrap.sock\n")
    certs = {}
    for _, identity in identities:
        cert = run_root / "certs" / (identity.strip("/").replace("/", "_") + ".cert")
        cert.parent.mkdir(parents=True, exist_ok=True)
        prefix = env_command(env_for(bootstrap_home, "bootstrap"), "ndnsec")
        subprocess.run(prefix + f" key-gen -t r {shlex.quote(identity)}", shell=True, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(prefix + f" cert-dump -i {shlex.quote(identity)} > {shlex.quote(str(cert))}",
                       shell=True, check=True)
        certs[identity] = cert
    for node, home in homes.items():
        shutil.copytree(bootstrap_home / "pib", home / "pib", dirs_exist_ok=True)
        shutil.copytree(bootstrap_home / "tpm", home / "tpm", dirs_exist_ok=True)
        with sqlite3.connect(home / "pib/pib.db") as conn:
            conn.execute("update tpmInfo set tpm_locator=?", ("tpm-file:" + str(home / "tpm"),))
            conn.commit()
        keep = {identity for owner, identity in identities if owner == node}
        with sqlite3.connect(home / "pib/pib.db") as conn:
            rows = conn.execute("select identities.identity, keys.key_name from identities join keys on keys.identity_id=identities.id").fetchall()
        for identity_blob, key_name in rows:
            try:
                from ndn.encoding import Component, Name
                parts, _ = Name.decode(bytes(identity_blob))
                identity = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
            except Exception:
                continue
            if identity in keep:
                continue
            (home / "tpm/ndnsec-key-file" / (hashlib.sha256(bytes(key_name)).hexdigest() + ".privkey")).unlink(missing_ok=True)
    for node, home in homes.items():
        default_identity = next(identity for owner, identity in identities if owner == node)
        prefix = env_command(env_for(home, node), "ndnsec")
        subprocess.run(prefix + f" set-default -n {shlex.quote(default_identity)}",
                       shell=True, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Find each provider's actual signing key prefix for NativeOfferAdmission.
    def key_prefix(index: int) -> str:
        db = homes[stage_nodes[index]] / "pib/pib.db"
        with sqlite3.connect(db) as conn:
            for (value,) in conn.execute("select key_name from keys"):
                raw = bytes(value)
                try:
                    from ndn.encoding import Component, Name
                    parts, _ = Name.decode(raw)
                    uri = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
                except Exception:
                    continue
                if uri.startswith(provider_names[index] + "/KEY/"):
                    return uri
        raise RuntimeError(f"provider key prefix unavailable: {provider_names[index]}")
    offer_entries = []; public_key_files = {}
    for index, provider in enumerate(provider_names):
        _, _, _, offer_public = provider_keys[index]
        offer_id = public_key_digest(offer_public)
        public_key_files[offer_id] = str(offer_public)
        prefix = key_prefix(index)
        offer_entries.append({"provider": provider, "service": SERVICE,
                              "keyLocatorPrefix": prefix,
                              "signerKeyId": offer_id,
                              "certificateName": prefix + "/ID-CERT"})
    candidate_digest = digest_text(f"{MODEL_FAMILY}-offer-policy")
    offer_policy = {"schema": "spec180-provider-offer-trust-v1", "candidateId": f"{MODEL_FAMILY}-local",
                    "candidateDigest": candidate_digest, "trustSchema": APP_ROOT + "/trust",
                    "entries": offer_entries}
    authority_cfg = {
        "schema": "ndnsf-di-native-authority-v1",
        "run_for_ms": runtime_budgets["authority_run_ms"],
        "permission_bootstrap_ms": 60000,
        "max_grant_ttl_ms": min(3600000, runtime_budgets["retention_ms"]),
        "authority": {"identity": AUTHORITY, "service": "/HELLO", "group": GROUP,
                       "controller_identity": CONTROLLER, "requester_identity": USER,
                       "protection_epoch": "epoch-1", "content_key_id": content_key_id,
                       "trust_schema_file": "../trust-schema.conf",
                       "authority_private_key_file": "authority-private.pem",
                       "requester_public_key_file": "../requester/requester-public.pem",
                       "content_key_file": str(content_key_path),
                       "allowed_model_manifests": [manifest_digest],
                       "recipient_public_key_files": {provider: str(provider_dirs[index] / "recipient-public.pem") for index, provider in enumerate(provider_names)},
                       "publication_sources": {manifest_digest: {
                           "model_name": model_descriptor["model_name"],
                           "model_content_digest": model_descriptor["content_digest"],
                           "canonical_source_digest": source_digest,
                           **({"initializer_object_digest": initializer_digest}
                              if initializer_digest else {}),
                           "artifact_profile_digest": catalog["recipe"]["artifact_profile_digest"]}}}}
    (authority_dir / "authority-private.pem").write_bytes(auth_private.read_bytes())
    (authority_dir / "requester-public.pem").write_bytes(req_public.read_bytes())
    for index, provider in enumerate(provider_names):
        provider_dirs[index].joinpath("recipient-public.pem").write_bytes(provider_keys[index][1].read_bytes())
    (authority_dir / "authority.json").write_text(json.dumps(authority_cfg, indent=2, sort_keys=True) + "\n")
    # NativeProtectedGrantCredentials resolves a complete operator-owned
    # registry from the Provider's authority-public-key hint.  Keep the
    # registry and its immutable public-key target in the run root, while
    # each Provider receives its own non-symlink hint and registry file.
    authority_public_bytes = auth_public.read_bytes()
    shared_authority_public = run_root / "authority-public.pem"
    shared_authority_public.write_bytes(authority_public_bytes)
    shared_authority_public.chmod(0o644)
    authority_registry = {
        "schemaVersion": 1,
        "status": "CONFIGURED",
        "artifactPolicyAuthority": {
            "acceptedModelFamilies": [MODEL_URI],
            "authorityId": AUTHORITY,
            "grantSchema": "ndnsf-di-key-grant-v1",
            "keyId": f"{MODEL_FAMILY}-artifact-policy-ed25519-local",
            "protectionEpochs": ["epoch-1"],
            "publicKeyAlgorithm": "ed25519",
            "publicKeyPath": "authority-public.pem",
            "publicKeySha256": digest_bytes(authority_public_bytes),
            "signatureAlgorithm": "ed25519",
        },
    }
    # Provider maps and requester config are written after key paths are stable.
    for index, provider in enumerate(provider_names):
        directory = provider_dirs[index]
        (directory / "plan.json").write_text(stage_plan.read_text())
        # Serving providers advertise role metadata and wait for the
        # authenticated Selection projection to bind a concrete artifact.
        # Remove exporter-local paths at this boundary: stage ONNX files are
        # contract fixtures, never preassembled Provider startup artifacts.
        provider_manifest = json.loads(service_manifest.read_text())
        for service in provider_manifest.get("services", []):
            for artifact in service.get("artifacts", []):
                artifact.pop("path", None)
                artifact["materialization"] = "post-selection-canonical-assembly"
        (directory / "manifest.json").write_text(
            json.dumps(provider_manifest, indent=2, sort_keys=True) + "\n")
        (directory / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
        (directory / "authority-public.pem").write_bytes(auth_public.read_bytes())
        (directory / "trust-root-registry-v1.json").write_text(
            json.dumps(authority_registry, indent=2, sort_keys=True) + "\n")
        (directory / "offer-private.pem").write_bytes(provider_keys[index][2].read_bytes())
        (directory / "recipient-private.pem").write_bytes(provider_keys[index][0].read_bytes())
        (directory / "recipient-key-map.json").write_text(json.dumps({provider: str(directory / "recipient-private.pem")}))
    encrypted_repository_path = None
    encrypted_repository_cleanup = None
    if not args.cache_compatibility_mode:
        encrypted_repository_path = resolve_encrypted_repository_path(
            run_root, args.encrypted_repository_path)
        encrypted_repository_cleanup = lambda: cleanup_encrypted_repository(
            encrypted_repository_path, run_root)
        atexit.register(encrypted_repository_cleanup)
    requester_cfg = {
        "schema": "ndnsf-di-native-requester-v1",
        "catalog": {**catalog, "source": {**catalog["source"], "file": source_path.name,
                                             **({"initializer_file": initializer_path.name}
                                                if initializer_path else {})}},
        "core": {"requester_identity": USER, "authority_identity": CONTROLLER,
                 "group": GROUP, "trust_schema_file": "../trust-schema.conf"},
        "grant": {"authority_identity": AUTHORITY, "authority_service": "/HELLO",
                   "authority_public_key_file": "authority-public.pem",
                   "requester_private_key_file": "requester-private.pem",
                   "protection_epoch": "epoch-1"},
        "offer_admission": {"policy": offer_policy, "public_key_files": public_key_files,
                            "candidate_digest": candidate_digest},
        # The canonical Repo is the prepare-time source owner/cache.  The
        # requester ingests the immutable source once; protected root/material
        # publication remains on Runtime's authenticated NDN LargeData path so
        # remote Providers can fetch the returned references.  Keep ciphertext
        # large data in its separate Repo below so the two ownership contracts
        # do not share cache accounting or cleanup.
        "repository": {"path": str(model_source_repository),
                        "max_bytes": 4 << 30,
                        "cache_namespace": source_cache_namespace,
                        "cache_identity": source_cache_identity,
                        "cache_hit": source_cache_hit},
        **({"cache_compatibility": {
                "enabled": True,
                "source_namespace": source_cache_namespace,
            }} if args.cache_compatibility_mode else {
                "encrypted_repository": {"path": str(encrypted_repository_path),
                                         "max_bytes": 4 << 30},
            }),
        "limits": {"bootstrap_ms": runtime_budgets["bootstrap_ms"],
                    "large_data_ims_limit": large_data_ims,
                    "max_source_bytes": max_source_bytes,
                    "max_assembled_bytes": max_assembled_bytes,
                    "max_prepared_bytes": max_prepared_bytes,
                    "max_prepared_entries": 8},
        "request": {"service": SERVICE, "task": "text-generation",
                     "adapter_composition_digest": adapter_digest,
                     "task_descriptor_digest": digest_text(f"{MODEL_FAMILY}-task"),
                     "generation_mode": "TOKEN_STREAMING", "tokenizer_digest": tokenizer_digest,
                     "input_layout_digest": digest_text(f"{MODEL_FAMILY}-input-layout"),
                     "security_policy_digest": digest_text(f"{MODEL_FAMILY}-security"),
                     # Assembly and authenticated material fetch happen after
                     # Selection and may be silent before the first token.
                     # Keep the bounded stream wait explicit in the generated
                     # native requester contract.
                     "interest_lifetime_ms": 5000, "max_event_retries": 8,
                     "max_candidates": 1, "max_policy_ms": runtime_budgets["policy_ms"], "provider_names": provider_names,
                     "max_reentries": 1, "no_progress_ms": runtime_budgets["no_progress_ms"],
                     "timeout_ms": runtime_budgets["timeout_ms"],
                     "ack_timeout_ms": runtime_budgets["ack_timeout_ms"],
                     "options_file": "options.json"},
        # Keep the two-node Qwen experiment deterministic: each published
        # pipeline stage is bound to its corresponding Provider.  The native
        # requester still resolves this through Runtime's registered custom
        # strategy, so the generic strategy remains available to other users.
        "fixed_placement": {"role_providers": {
            stage["role"]: provider_names[index]
            for index, stage in enumerate(stages)}},
        "conversation": {"schema": "ndnsf-di-native-conversation-v1",
                          "journal": {"state_root": "conversation-state", "identity": f"{MODEL_FAMILY}-user",
                                      "keys": [{"id": "active", "file": "conversation.key"}],
                                      "quota_bytes": NATIVE_CONVERSATION_JOURNAL_MAX_BYTES},
                          "owner": {"requester_identity": USER, "service_name": SERVICE,
                                    "security_domain_digest": digest_text(f"{MODEL_FAMILY}-security")},
                          "turn": {"conversation_id": f"{MODEL_FAMILY}-minindn-conversation",
                                   "parent_context_epoch": 0, "service_name": SERVICE,
                                   "plan_role_map_digest": role_map_digest,
                                   "retention_deadline_ms": int(time.time() * 1000) + runtime_budgets["retention_ms"],
                                   "mode": "FULL_CONTEXT", "generation_id": "0123456789abcdef0123456789abcdef",
                                   "canonical_token_ids": input_ids,
                                   "expected_roles": [stage["role"] for stage in stages]},
                          "checkpoint_output_file": "conversation-state.json"},
    }
    (requester_dir / "requester-private.pem").write_bytes(req_private.read_bytes())
    (requester_dir / "authority-public.pem").write_bytes(auth_public.read_bytes())
    (requester_dir / "conversation.key").write_bytes(os.urandom(32))
    (requester_dir / "conversation.key").chmod(0o600)
    (requester_dir / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    (requester_dir / "options.json").write_text(json.dumps({
        "useCache": True, "outputMode": "TOKEN_STREAMING",
        "generationId": "0123456789abcdef0123456789abcdef",
        "maxNewTokens": args.max_new_tokens, "tokenizerDigest": tokenizer_digest,
        "eosTokenIds": eos_token_ids, "sampling": {"mode": "Greedy", "temperature": 0.0,
        "topK": 1, "topP": 1.0, "repetitionPenalty": 1.0, "seed": 18406},
        "stopStrings": [], "tokenInputName": "input_ids",
        "stateInputNames": all_state_inputs,
        "stateOutputNames": all_state_outputs,
        "stateSuccessorMap": ",".join(
            f"{input_name}={output_name}"
            for input_name, output_name in canonical_qwen_state_successor_pairs(stages)),
        "positionInputPolicy": f"{MODEL_FAMILY}-causal-position-v1",
        "attentionMaskInputName": "attention_mask",
        "positionIdsInputName": "position_ids",
    }, indent=2, sort_keys=True) + "\n")
    # One native requester owns the complete conversation.  Each entry names
    # only its input/options/output files; Conversation owns the authenticated
    # checkpoint and KV lineage between entries.
    turns = [{"input_file": "input.bin", "options_file": "options.json",
              "output_file": "output-0.bin",
              "checkpoint_output_file": "conversation-state.json"}]
    for round_index in range(1, args.rounds):
        options = continuation_options(
            json.loads((requester_dir / "options.json").read_text()), round_index)
        (requester_dir / f"options-{round_index}.json").write_text(
            json.dumps(options, indent=2, sort_keys=True) + "\n")
        turns.append({"input_file": "delta.bin",
                      "options_file": f"options-{round_index}.json",
                      "output_file": f"output-{round_index}.bin",
                      "checkpoint_output_file": f"conversation-state-{round_index}.json"})
    requester_cfg["turns"] = turns
    (requester_dir / "config.json").write_text(json.dumps(requester_cfg, indent=2, sort_keys=True) + "\n")
    # Preserve the established C++ oracle's per-round config view while the
    # production requester itself is launched exactly once.
    for round_index in range(1, args.rounds):
        compatibility = continuation_config(requester_cfg, round_index, delta_ids)
        (requester_dir / f"config-{round_index}.json").write_text(
            json.dumps(compatibility, indent=2, sort_keys=True) + "\n")
    # Re-check every identity immediately before creating MiniNDN processes.
    # The earlier preflight protects preparation; this final fence prevents a
    # replacement between preflight/materialization and the actual requester,
    # provider, or controller exec from changing the candidate in flight.
    require_file_digest(stage_manifest_path, args.stage_manifest_sha256, "MODEL_STAGE_MANIFEST")
    load_stage_manifest(stage_manifest_path, args.stage_root)
    require_file_digest(args.topology, args.topology_sha256, "TOPOLOGY")
    require_file_digest(tokenizer, args.tokenizer_sha256, "MODEL_TOKENIZER")
    require_file_digest(receipt, args.build_receipt_sha256, "BUILD_RECEIPT")
    for path, expected, label in binary_checks:
        require_file_digest(path, expected, label)
    require_file_digest(source_path, args.canonical_source_sha256, "MODEL_CANONICAL_SOURCE")
    require_file_digest(mapping_path, args.node_mapping_sha256, "MODEL_NODE_MAPPING")
    if initializer_path is not None:
        require_file_digest(initializer_path, args.canonical_initializer_sha256,
                           "MODEL_CANONICAL_INITIALIZER")
    # MiniNDN and process orchestration starts here.
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    # MiniNDN's constructor parses the process argv itself.  Remove runner
    # options after our parser has consumed them so its CLI never sees them.
    sys.argv = [sys.argv[0]]
    Minindn.cleanUp()
    ndn = Minindn(topoFile=str(args.topology.expanduser().resolve()))
    node_app_plan = mini_ndn_node_app_plan(
        {host.name for host in ndn.net.hosts}, args.controller_node,
        args.user_node, stage_nodes, provider_names)
    (run_root / "minindn-node-app-plan.json").write_text(
        json.dumps(node_app_plan, indent=2, sort_keys=True) + "\n")
    processes = []
    provider_logs = []
    ndn_started = False
    cache_purge_completed = False
    try:
        ndn.start()
        ndn_started = True
        AppManager(ndn, ndn.net.hosts, Nfd, csSize=MININDN_NFD_CS_SIZE,
                   logLevel="INFO")
        # Mini-NDN's upstream examples use Nlsr and NdnRoutingHelper as
        # alternative routing owners. This experiment needs deterministic
        # application-prefix routes, so use the static helper only; starting
        # Nlsr as well would duplicate UDP faces and FIB ownership.
        rh = NdnRoutingHelper(ndn.net, Nfdc.PROTOCOL_UDP, "link-state")
        rh.addOrigin([ndn.net[args.controller_node]], [CONTROLLER, CONTROLLER + "/KEY", AUTHORITY, AUTHORITY + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node]], [USER, USER + "/KEY"])
        for node, provider in zip(stage_nodes, provider_names):
            rh.addOrigin([ndn.net[node]], [provider, provider + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node], *[ndn.net[node] for node in stage_nodes]], [GROUP])
        rh.calculateRoutes()
        time.sleep(max(0.0, args.routing_wait_s))
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP, Nfdc.STRATEGY_MULTICAST)
        def start(node_name: str, label: str, command: str, env: dict[str, str]):
            log = run_root / f"{label}.log"
            handle = log.open("wb")
            proc = getPopen(ndn.net[node_name], command, envDict=env, shell=True,
                            stdout=handle, stderr=subprocess.STDOUT)
            processes.append((proc, handle, log))
            return proc, log
        authority_env = env_for(homes[args.controller_node], args.controller_node)
        # Bind Controller's fenced generation store to this immutable run.
        # The default global /tmp path can retain a stale writer lock after a
        # manually interrupted MiniNDN run and would poison later candidates.
        authority_env["NDNSF_CONTROLLER_GENERATION_STATE"] = str(
            run_root / "controller-generation.state")
        # App_ServiceController keeps the maintained trust-schema default as a
        # repository-relative path.  MiniNDN launches commands from a host
        # namespace working directory, so make that dependency explicit while
        # keeping all generated run artifacts absolute and portable.
        revoke_trigger_path = run_root / "controller-revoke.trigger" \
            if args.revoke_after_first_round else None
        controller_revoke_args = ""
        if revoke_trigger_path is not None:
            controller_revoke_args = (
                f" --revoke-trigger-file {shlex.quote(str(revoke_trigger_path))}"
                f" --revoke-kind identity --revoke-identity {shlex.quote(USER)}")
        controller_cmd = (f"cd {shlex.quote(str(ROOT))} && exec "
                          f"{shlex.quote(str(args.controller_binary.expanduser().resolve()))} "
                          f"--controller-prefix {shlex.quote(CONTROLLER)} --policy-file {shlex.quote(str(policy))} "
                          f"--ensure-identities {shlex.quote(AUTHORITY + ',' + ','.join(provider_names) + ',' + USER)} "
                          f"--no-serve-certificates{controller_revoke_args} "
                          f"--run-for-ms {runtime_budgets['authority_run_ms']}")
        controller_proc, controller_log = start(args.controller_node, "controller", controller_cmd, authority_env)
        # The banner is emitted before Controller::start() enters its real
        # PUBPARAMS readiness probe.  Wait for that process to reach the
        # banner, then give its registration path a bounded settle window
        # before launching the Authority.  Authority must still start before
        # Controller::start() can finish its reciprocal probe.
        wait_for_marker(controller_proc, controller_log, ("ServiceController started",), args.startup_timeout_s)
        time.sleep(0.5)
        authority_cmd = (f"{shlex.quote(str(authority_binary))} "
                         f"--config {shlex.quote(str(authority_dir / 'authority.json'))}")
        authority_proc, authority_log = start(args.controller_node, "authority", authority_cmd, authority_env)
        wait_for_marker(authority_proc, authority_log, ("NATIVE_GRANT_AUTHORITY_READY",), args.startup_timeout_s)
        for index, node in enumerate(stage_nodes):
            provider = provider_names[index]; directory = provider_dirs[index]
            env = env_for(homes[node], node)
            env.update({"SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(directory / "authority-public.pem"),
                        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(directory / "recipient-key-map.json"),
                        "NDNSF_DI_WORKER_BINARY": str(assembly_worker_binary)})
            command = (f"{shlex.quote(str(provider_binary))} --plan {shlex.quote(str(directory / 'plan.json'))} "
                       f"--manifest {shlex.quote(str(directory / 'manifest.json'))} --service {shlex.quote(SERVICE)} "
                       f"--provider {shlex.quote(provider)} --group {shlex.quote(GROUP)} --controller {shlex.quote(CONTROLLER)} "
                       f"--trust-schema {shlex.quote(str(directory / 'trust-schema.conf'))} --roles {shlex.quote(stages[index]['role'])} "
                       f"--serve --run-for-ms {runtime_budgets['provider_run_ms']} --artifact-cache-dir "
                       f"{shlex.quote(str(artifact_cache_root / provider_cache_namespace(provider)))} "
                       f"--repo-fetch-timeout-ms {runtime_budgets['timeout_ms']} "
                       f"--assembly-timeout-ms {runtime_budgets['assembly_timeout_ms']} "
                       f"--conversation-retention-ms {min(3600000, runtime_budgets['retention_ms'])} "
                       f"--tokenizer-json {shlex.quote(str(tokenizer))} --selection-offer-key-file {shlex.quote(str(directory / 'offer-private.pem'))} "
                       "--offer-backend onnxruntime-cpu --offer-can-provision --offer-has-model" +
                       (f" --cache-compatibility-source-dir {shlex.quote(str(model_source_repository))}"
                        if args.cache_compatibility_mode else ""))
            proc, log = start(node, f"provider-{index}", command, env)
            provider_logs.append(log)
            wait_for_marker(proc, log, ("NDNSF_DI_NATIVE_PROVIDER_READY", "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY"), args.startup_timeout_s)
        requester_env = env_for(homes[args.user_node], args.user_node)
        # Large canonical objects require an explicit, finite requester-side
        # IMS reservation.  Providers do not publish the canonical source and
        # retain the historical default, so this opt-in cannot enlarge every
        # process in a deployment.
        requester_env["NDNSF_REQUEST_LARGE_IMS_LIMIT"] = str(large_data_ims)
        def requester_launch(config_path: Path):
            launch_env = dict(requester_env)
            if requester_driver_binary is not None:
                # The driver is a C++ parent/pipe oracle.  It launches the
                # verified requester itself after observing a live event;
                # keeping the two identities separate prevents a test driver
                # from replacing the production requester preflight.
                launch_env["SPEC190_NATIVE_REQUESTER_BINARY"] = str(requester_binary)
                launch_env["SPEC190_NATIVE_TURNS_CONFIG"] = str(config_path)
                # The Waf unit-test executable contains the repository's
                # other integration suites as well.  Select only the named
                # parent/pipe case so unrelated fixture requirements cannot
                # turn a live MiniNDN run into an aggregate-test result.
                driver_cmd = (
                    f"{shlex.quote(str(requester_driver_binary))} "
                    "--run_test=Spec190LiveTurns/ParentPipeReadsLiveEventsBeforeTerminal")
                return driver_cmd, launch_env
            return (f"{shlex.quote(str(requester_binary))} --config "
                    f"{shlex.quote(str(config_path))}"), launch_env

        revocation_record = None
        provider_measurement = {}
        requester_measurement = {}
        requester_config = requester_dir / "config.json"
        if args.revoke_after_first_round:
            # Keep the production requester and continuation contract intact,
            # but split the two process launches only for this deterministic
            # revocation gate.  The second config reads the first process's
            # authenticated checkpoint while Provider-owned resident state
            # remains in the same Provider processes.
            full_config = json.loads(requester_config.read_text())
            first_config = json.loads(json.dumps(full_config))
            first_config["turns"] = [full_config["turns"][0]]
            first_config_path = requester_dir / "config-revoke-first.json"
            first_config_path.write_text(
                json.dumps(first_config, indent=2, sort_keys=True) + "\n")
            continuation = json.loads(
                (requester_dir / "config-1.json").read_text())
            continuation["turns"] = [full_config["turns"][1]]
            continuation_path = requester_dir / "config-revoke-continuation.json"
            continuation_path.write_text(
                json.dumps(continuation, indent=2, sort_keys=True) + "\n")
            requester_cmd, requester_launch_env = requester_launch(first_config_path)
        else:
            requester_cmd, requester_launch_env = requester_launch(requester_config)
        placement = {log: (provider_names[index], stages[index]["role"])
                     for index, log in enumerate(provider_logs)}
        tail_role = stages[-1]["role"]
        round_deadline = time.monotonic() + runtime_budgets["process_timeout_s"]
        provider_offsets = snapshot_provider_logs(provider_logs)
        first_proc, first_log = start(args.user_node, "requester", requester_cmd, requester_launch_env)
        if args.revoke_after_first_round:
            first_text = wait_for_native_round(
                first_proc, first_log, provider_logs,
                max(0.0, round_deadline - time.monotonic()))
            if (first_proc.returncode != 0 or
                    first_text.count("NATIVE_REQUEST_SUCCEEDED") != 1 or
                    first_text.count("NATIVE_CONVERSATION_TURN_COMPLETE") != 1 or
                    "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN" not in first_text):
                raise RuntimeError(f"native Qwen first revocation round failed: {first_log}")
            turn_texts = split_native_turn_logs(first_text, 1)
            (run_root / "requester-0.log").write_text(turn_texts[0])
            process_barrier = wait_for_provider_round_barrier(
                turn_texts[0], provider_offsets, placement, tail_role, round_deadline)
            first_provider_delta = provider_log_delta(provider_offsets)
            requester_measurement = phase_timing_summary(first_text)
            provider_measurement = {
                str(path): {"phaseTiming": phase_timing_summary(text),
                            "onnxSessionLoads": onnx_session_load_markers(text)}
                for path, text in first_provider_delta.items()}
            resident_manifest_values = []
            for directory in provider_dirs:
                manifest = json.loads((directory / "manifest.json").read_text())
                resident_manifest_values.extend(manifest_resident_session_values(manifest))
            resident_runner_ready = all(
                "phase=CACHE_LOOKUP_HIT" in text and
                "stage=RUNNER_READY" in text
                for text in first_provider_delta.values())
            resident_session_bound = (
                bool(resident_manifest_values) and
                all(value == "true" for value in resident_manifest_values) and
                resident_runner_ready)
            if not resident_session_bound:
                raise RuntimeError(
                    "resident-session evidence missing manifest opt-in, cache hit, or RUNNER_READY")
            if revoke_trigger_path is None:
                raise RuntimeError("revocation trigger path was not initialized")
            revoke_trigger_path.write_text("spec190-revoke-after-first-round\n")
            revoke_trigger_path.chmod(0o600)
            wait_for_marker(controller_proc, controller_log,
                            ("NDNSF_REVOCATION_APPLIED success=1",),
                            args.startup_timeout_s + 60.0)
            controller_text = controller_log.read_text(errors="replace")
            if "NDNSF_REVOCATION_TRIGGERED" not in controller_text:
                raise RuntimeError("controller revocation trigger was not consumed")
            revoke_matches = re.findall(
                r"NDNSF_REVOCATION_APPLIED success=1 kind=(\d+) "
                r"generation=(\d+) epoch=(\d+)", controller_text)
            if len(revoke_matches) != 1:
                raise RuntimeError("controller revocation generation/epoch evidence is ambiguous")
            revoke_kind, revoke_generation, revoke_epoch = revoke_matches[0]

            continuation_provider_offsets = snapshot_provider_logs(provider_logs)
            continuation_cmd, continuation_env = requester_launch(continuation_path)
            continuation_proc, continuation_log = start(
                args.user_node, "requester-revoked", continuation_cmd, continuation_env)
            continuation_deadline = time.monotonic() + max(
                60.0, args.startup_timeout_s + 120.0)
            while continuation_proc.poll() is None and time.monotonic() < continuation_deadline:
                time.sleep(0.2)
            if continuation_proc.poll() is None:
                continuation_proc.send_signal(signal.SIGTERM)
                try:
                    continuation_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    continuation_proc.kill()
                    continuation_proc.wait(timeout=5)
                raise RuntimeError(
                    f"revoked Qwen continuation did not terminate: {continuation_log}")
            continuation_text = continuation_log.read_text(errors="replace")
            if continuation_proc.returncode == 0 or "NATIVE_REQUEST_SUCCEEDED" in continuation_text:
                raise RuntimeError(
                    f"revoked Qwen continuation unexpectedly succeeded: {continuation_log}")
            required_failure = (
                "NATIVE_REQUEST_STAGE_FAILED code=PREPARATION_FAILED boundary=preparation" in
                continuation_text and
                "PROTECTED_CONTROLLER_VERSION_UNAVAILABLE" in continuation_text)
            if not required_failure:
                raise RuntimeError(
                    f"revoked Qwen continuation missed the exact protected-version failure: {continuation_log}")
            continuation_provider_delta = provider_log_delta(continuation_provider_offsets)
            forbidden_provider_markers = (
                "NDNSF_DI_NATIVE_SELECTION_ACCEPTED",
                "stage=EXECUTION_ENTERED",
                "stage=DEPENDENCY_FETCH",
                "stage=ASSEMBLY_STARTED",
                "stage=RUNNER_READY",
                "stage=EXECUTION_COMPLETED",
                "stage=TERMINAL",
                "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED",
            )
            provider_execution_after_revoke = {
                str(path): [marker for marker in forbidden_provider_markers if marker in text]
                for path, text in continuation_provider_delta.items()
                if any(marker in text for marker in forbidden_provider_markers)
            }
            if provider_execution_after_revoke:
                raise RuntimeError(
                    "revoked continuation reached a Provider execution stage: " +
                    json.dumps(provider_execution_after_revoke, sort_keys=True))
            oracle_record = run_cpp_revocation_oracle(
                oracle_binary, run_root, max(30.0, args.startup_timeout_s))
            first_output = requester_dir / "output-0.bin"
            round_records = [{"round": 0, "returncode": first_proc.returncode,
                              "log": str(run_root / "requester-0.log"),
                              "output": str(first_output),
                              "providerFinalization": process_barrier,
                              "phaseTiming": phase_timing_summary(turn_texts[0]),
                              **validate_native_output(first_output)}]
            round_records.append({"round": "revoked-continuation",
                                  "returncode": continuation_proc.returncode,
                                  "log": str(continuation_log),
                                  "outcome": "FAIL_CLOSED"})
            revocation_record = {
                "status": "REVOCATION_FAIL_CLOSED_PASS",
                "controllerLog": str(controller_log),
                "trigger": str(revoke_trigger_path),
                "target": USER,
                "continuationLog": str(continuation_log),
                "continuationReturncode": continuation_proc.returncode,
                "controllerRevocation": {"kind": int(revoke_kind),
                                          "generation": int(revoke_generation),
                                          "epoch": int(revoke_epoch)},
                "continuationFailureCode": "PREPARATION_FAILED",
                "continuationFailureBoundary": "preparation",
                "providerExecutionAfterRevoke": provider_execution_after_revoke,
                "residentSessionRequested": True,
                "residentSessionEvidence": {
                    "manifestResidentSession": resident_manifest_values,
                    "providerCacheLookupHit": True,
                    "providerRunnerReady": True,
                    "runtimeLoadCount": "UNOBSERVED",
                },
                "residentRunnerWasWarmBeforeRevoke": resident_session_bound,
            }
        else:
            first_text = wait_for_native_round(
                first_proc, first_log, provider_logs,
                max(0.0, round_deadline - time.monotonic()))
            if (first_proc.returncode != 0 or
                    first_text.count("NATIVE_REQUEST_SUCCEEDED") != args.rounds or
                    (args.rounds > 1 and
                     "NATIVE_CONVERSATION_TURNS_SUCCEEDED" not in first_text) or
                    first_text.count("NATIVE_CONVERSATION_TURN_COMPLETE") != args.rounds or
                    "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN" not in first_text):
                raise RuntimeError(f"native Qwen turns failed: {first_log}")
            turn_texts = split_native_turn_logs(first_text, args.rounds)
            for round_index, turn_text in enumerate(turn_texts):
                (run_root / f"requester-{round_index}.log").write_text(turn_text)
            process_barrier = wait_for_provider_round_barrier(
                turn_texts[-1], provider_offsets, placement, tail_role, round_deadline)
            provider_delta = provider_log_delta(provider_offsets)
            requester_measurement = phase_timing_summary(first_text)
            provider_measurement = {
                str(path): {"phaseTiming": phase_timing_summary(text),
                            "onnxSessionLoads": onnx_session_load_markers(text)}
                for path, text in provider_delta.items()}
            round_records = []
            for round_index in range(args.rounds):
                output_path = requester_dir / f"output-{round_index}.bin"
                output_summary = validate_native_output(output_path)
                turn_request_id = conversation_turn_request_id(turn_texts[round_index])
                round_records.append({"round": round_index, "returncode": first_proc.returncode,
                                      "requestId": turn_request_id,
                                      "log": str(run_root / f"requester-{round_index}.log"),
                                      "output": str(output_path),
                                      "providerFinalization": process_barrier if round_index == args.rounds - 1 else {
                                          "observation": "covered-by-final-process-barrier"},
                                      "phaseTiming": phase_timing_summary(
                                          turn_texts[round_index], turn_request_id),
                                      **output_summary})
        if args.negative_parent:
            cfg = json.loads((requester_dir / "config.json").read_text())
            cfg["conversation"]["turn"]["parent_checkpoint_digest"] = "sha256:" + "0" * 64
            bad = requester_dir / "config-negative.json"; bad.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
            negative_cmd, negative_env = requester_launch(bad)
            proc, log = start(args.user_node, "requester-negative",
                              negative_cmd, negative_env)
            proc.wait(timeout=120); text = log.read_text(errors="replace")
            if proc.returncode == 0 or "DI_NATIVE_CONVERSATION_PARENT_MISMATCH" not in text:
                raise RuntimeError(f"negative parent case did not fail closed: {log}")
            round_records.append({"round": "negative-parent", "returncode": proc.returncode, "log": str(log)})
        # Every round crosses the observation barrier; the C++ oracle checks
        # each request identity, generation result, and chained KV checkpoint.
        if not args.revoke_after_first_round:
            oracle_record = run_cpp_oracle(oracle_binary, run_root,
                                          args.cache_compatibility_mode,
                                          max(30.0, args.startup_timeout_s), args.require_multi_token,
                                          args.rounds)
        # Keep authenticated material available for every continuation turn.
        # The large-data CS is purged only after the final Provider barrier and
        # the native oracle, never when the first runner becomes ready.
        purge_minindn_large_data_cache(ndn.net.hosts, run_root)
        cache_purge_completed = True
        record = {"schema": f"ndnsf-di-{MODEL_FAMILY}-native-minindn-run-v1", "model": model_name,
                  "revision": revision, "stageManifest": str(stage_manifest_path),
                  "stageManifestDigest": manifest_digest, "tokenizerDigest": tokenizer_digest,
                  "topology": str(args.topology.expanduser().resolve()), "stageNodes": stage_nodes,
                  "largeDataImsLimit": large_data_ims,
                  "runtimeBudgets": runtime_budgets,
                  "rounds": round_records,
                  "requesterPhaseTiming": requester_measurement,
                  "providerMeasurement": provider_measurement,
                  "cppOracle": oracle_record,
                  "status": ("PASS" if revocation_record is not None and
                             oracle_record["status"] == "PASS"
                             else oracle_record["status"])}
        if revocation_record is not None:
            record["revocation"] = revocation_record
        if args.cache_compatibility_mode:
            record["fullPathQualification"] = "NOT_RUN"
        (run_root / "run-record.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        print(f"NDNSF_DI_{MODEL_FAMILY.upper()}_NATIVE_MININDN_{record['status']} " + json.dumps(record, sort_keys=True))
        return 0
    finally:
        if ndn_started and not cache_purge_completed:
            try:
                purge_minindn_large_data_cache(
                    ndn.net.hosts, run_root, trigger="failure-cleanup")
                cache_purge_completed = True
            except Exception as purge_error:
                (run_root / "ndn-cache-purge-failure.json").write_text(
                    json.dumps({"schema": "ndnsf-minindn-large-data-cache-purge-failure-v1",
                                "trigger": "failure-cleanup",
                                "error": repr(purge_error)},
                               indent=2, sort_keys=True) + "\n")
        for proc, handle, _ in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
        for proc, handle, _ in reversed(processes):
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=2)
            handle.close()
        ndn.stop()
        if encrypted_repository_cleanup is not None:
            try:
                encrypted_repository_cleanup()
            finally:
                atexit.unregister(encrypted_repository_cleanup)


if __name__ == "__main__":
    raise SystemExit(main())
