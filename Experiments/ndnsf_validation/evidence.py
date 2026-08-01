"""Admission checks for real Qwen generation campaign evidence."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class EvidenceError(ValueError):
    pass


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise EvidenceError(f"generation evidence is absent: {path}")
    rows: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise EvidenceError(
                f"invalid generation JSONL line {line_number}: {exc}"
            ) from exc
        if not isinstance(value, dict):
            raise EvidenceError(f"generation line {line_number} is not an object")
        rows.append(value)
    return rows


def validate_generation_evidence(
    rows: list[dict[str, Any]],
    *,
    workload: dict[str, Any],
    require_lineage: bool = True,
) -> dict[str, Any]:
    prompt_ids = {str(item["promptId"]) for item in workload["prompts"]}
    warmup = int(workload["warmupPerPrompt"])
    measured = int(workload["measuredPerPrompt"])
    minimum_tokens = int(workload["minimumGeneratedTokens"])
    expected_count = len(prompt_ids) * (warmup + measured)
    if len(rows) != expected_count:
        raise EvidenceError(
            f"expected {expected_count} generation rows, found {len(rows)}"
        )
    keys: set[tuple[str, str, int]] = set()
    measured_rows: list[dict[str, Any]] = []
    for row in rows:
        if row.get("schemaVersion") != "ndnsf-di-qwen-generation-sample-v1":
            raise EvidenceError("unsupported generation evidence schema")
        prompt_id = str(row.get("promptId", ""))
        phase = str(row.get("phase", ""))
        repetition = int(row.get("repetition", -1))
        key = (prompt_id, phase, repetition)
        if prompt_id not in prompt_ids:
            raise EvidenceError(f"unexpected prompt identity: {prompt_id}")
        if phase not in {"warmup", "measured"}:
            raise EvidenceError(f"unexpected phase: {phase}")
        limit = warmup if phase == "warmup" else measured
        if repetition < 0 or repetition >= limit or key in keys:
            raise EvidenceError(f"duplicate or invalid generation key: {key}")
        keys.add(key)
        if row.get("status") != "OK":
            raise EvidenceError(
                f"generation {row.get('generationId', key)} did not pass"
            )
        token_ids = row.get("generatedTokenIds")
        token_steps = row.get("tokenSteps")
        if not isinstance(token_ids, list) or not isinstance(token_steps, list):
            raise EvidenceError("generation token evidence must be lists")
        if len(token_ids) < minimum_tokens or len(token_steps) != len(token_ids):
            raise EvidenceError("generation does not meet token-event minimum")
        if not str(row.get("decodedText", "")).strip():
            raise EvidenceError("generation decoded answer is empty")
        if float(row.get("ttftMs", 0)) <= 0:
            raise EvidenceError("generation TTFT must be positive")
        if float(row.get("totalMs", 0)) <= 0:
            raise EvidenceError("generation total latency must be positive")
        if float(row.get("tokensPerSecond", 0)) <= 0:
            raise EvidenceError("generation tokens/s must be positive")
        inter_token = row.get("interTokenMs")
        if not isinstance(inter_token, list) or len(inter_token) != len(token_ids) - 1:
            raise EvidenceError("inter-token latency count is incomplete")
        if require_lineage:
            expected_model = workload["modelIdentity"]["contentDigest"]
            if row.get("modelIdentityDigest") != expected_model:
                raise EvidenceError("generation model identity mismatch")
            if row.get("workloadDigest") != workload["workloadDigest"]:
                raise EvidenceError("generation workload identity mismatch")
            for token_epoch, step in enumerate(token_steps):
                request_id = str(step.get("requestId", ""))
                expected = f"{row['generationId']}-token-{token_epoch}"
                if request_id != expected:
                    raise EvidenceError("token request ID lineage mismatch")
                transport = step.get("transport")
                if not isinstance(transport, dict):
                    raise EvidenceError("token transport lineage is absent")
                if str(transport.get("wireRequestId", "")) != request_id:
                    raise EvidenceError("wire request ID lineage mismatch")
                if int(transport.get("attempt", -1)) != 1:
                    raise EvidenceError("token attempt lineage mismatch")
                if str(transport.get("planId", "")) != str(
                    row.get("planId", "")
                ):
                    raise EvidenceError("token plan lineage mismatch")
                if transport.get("modelIdentityDigest") != expected_model:
                    raise EvidenceError("token model lineage mismatch")
        if phase == "measured":
            measured_rows.append(row)
    expected_measured = len(prompt_ids) * measured
    if len(measured_rows) != expected_measured:
        raise EvidenceError(
            f"expected {expected_measured} measured rows, found {len(measured_rows)}"
        )
    return {
        "rowCount": len(rows),
        "warmupCount": len(rows) - len(measured_rows),
        "measuredCount": len(measured_rows),
        "minimumObservedTokens": min(
            len(row["generatedTokenIds"]) for row in measured_rows
        ),
        "allMeasuredPassed": True,
    }
