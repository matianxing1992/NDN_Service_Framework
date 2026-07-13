#!/usr/bin/env python3
"""Spec 107 fail-before-role-start campaign preflight tests."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec107_identity import (  # noqa: E402
    build_candidate_identity,
    build_campaign_identity,
)
from spec107_preflight import (  # noqa: E402
    PreflightError,
    claim_campaign_writer,
    run_campaign_preflight,
    write_invalid_preflight_record,
)


def digest(character: str) -> str:
    return "sha256:" + character * 64


def candidate(artifact_digest: str | None = None) -> dict[str, object]:
    values = {
        "source": digest("1"), "profile": digest("2"),
        "model": digest("3"), "plan": digest("4"),
        "artifact": artifact_digest or digest("5"), "lineage": digest("6"),
        "workload": digest("7"), "tokenizer": digest("8"),
        "trustPolicy": digest("9"), "command": digest("a"),
    }
    return build_candidate_identity(values, created_at="2026-07-12T00:00:00Z")


class Spec107PreflightTest(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[dict[str, object], dict[str, object], Path, dict[str, object]]:
        artifact_root = root / "results/spec107-artifacts/set-a"
        artifact_root.mkdir(parents=True, exist_ok=True)
        artifact = artifact_root / "stage-0.onnx"
        artifact.write_bytes(b"model-stage-0")
        artifact_digest = "sha256:" + hashlib.sha256(artifact.read_bytes()).hexdigest()
        manifest: dict[str, object] = {
            "schema": "ndnsf-di-spec107-artifact-set-v1",
            "artifactSetDigest": digest("5"),
            "artifacts": [{
                "role": "/LLM/Stage/0",
                "path": "stage-0.onnx",
                "bytes": artifact.stat().st_size,
                "sha256": artifact_digest,
            }],
        }
        manifest_path = artifact_root / "artifact-set.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        manifest_digest = "sha256:" + hashlib.sha256(
            manifest_path.read_bytes()).hexdigest()
        value = candidate(manifest_digest)
        campaign = build_campaign_identity(
            value,
            kind="performance",
            ordinal=1,
            command_digest=digest("b"),
            output_root="results/spec107-c1-performance-r1",
        )
        return value, campaign, artifact_root, manifest

    def _run(self, root: Path, **overrides: object) -> dict[str, object]:
        value, campaign, artifact_root, manifest = self._fixture(root)
        arguments: dict[str, object] = {
            "candidate": value,
            "campaign": campaign,
            "artifact_root": artifact_root,
            "artifact_manifest": manifest,
            "repo_root": root,
            "projected_new_bytes": 4096,
            "reserve_bytes": 1024,
            "free_bytes": 8192,
            "expected_uid": os.getuid(),
            "provider_capabilities": {
                "/provider/0": ["qwen-generation-session-v1"],
                "/provider/1": ["qwen-generation-session-v1"],
                "/provider/2": ["qwen-generation-session-v1"],
            },
        }
        arguments.update(overrides)
        return run_campaign_preflight(**arguments)  # type: ignore[arg-type]

    def test_passes_only_when_every_check_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = self._run(Path(tmp))
            self.assertEqual(result["verdict"], "PASS")
            self.assertTrue(result["roleStartAllowed"])
            self.assertEqual(result["reasons"], [])
            self.assertEqual(result["requiredBytes"], 5120)
            self.assertEqual(result["freeBytes"], 8192)

    def test_existing_output_or_stale_writer_blocks_before_directory_creation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "results/spec107-c1-performance-r1"
            output.mkdir(parents=True)
            existing = self._run(root)
            self.assertEqual(existing["verdict"], "INVALID_PREFLIGHT")
            self.assertIn("OUTPUT_EXISTS", existing["reasons"])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            marker = root / "results/spec107-c1-performance-r1.writer.json"
            marker.parent.mkdir(parents=True)
            marker.write_text(json.dumps({"pid": 999999999}), encoding="utf-8")
            stale = self._run(root)
            self.assertEqual(stale["verdict"], "INVALID_PREFLIGHT")
            self.assertIn("OUTPUT_STALE_WRITER", stale["reasons"])
            self.assertFalse((root / "results/spec107-c1-performance-r1").exists())

    def test_owner_artifact_hash_and_artifact_size_mismatch_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            owner = self._run(root, expected_uid=os.getuid() + 1)
            self.assertIn("OUTPUT_PARENT_OWNER_MISMATCH", owner["reasons"])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            value, campaign, artifact_root, manifest = self._fixture(root)
            (artifact_root / "stage-0.onnx").write_bytes(b"tampered")
            result = run_campaign_preflight(
                candidate=value, campaign=campaign,
                artifact_root=artifact_root, artifact_manifest=manifest,
                repo_root=root, projected_new_bytes=1, reserve_bytes=1,
                free_bytes=100, provider_capabilities={
                    "/provider/0": ["qwen-generation-session-v1"]},
            )
            self.assertIn("ARTIFACT_DIGEST_MISMATCH:stage-0.onnx", result["reasons"])
            self.assertIn("ARTIFACT_SIZE_MISMATCH:stage-0.onnx", result["reasons"])

    def test_disk_projection_requires_strict_projected_plus_reserve_capacity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exact = self._run(root, projected_new_bytes=4096, reserve_bytes=1024,
                              free_bytes=5120)
            self.assertEqual(exact["verdict"], "INVALID_PREFLIGHT")
            self.assertIn("INSUFFICIENT_FREE_SPACE", exact["reasons"])

            negative = self._run(root, projected_new_bytes=-1)
            self.assertIn("PROJECTED_BYTES_INVALID", negative["reasons"])

    def test_mixed_or_missing_generation_capability_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self._run(root, provider_capabilities={
                "/provider/0": ["qwen-generation-session-v1"],
                "/provider/1": [],
            })
            self.assertEqual(result["verdict"], "INVALID_PREFLIGHT")
            self.assertEqual(
                result["reasons"],
                ["PROVIDER_CAPABILITY_MISSING:/provider/1:qwen-generation-session-v1"],
            )

    def test_invalid_record_is_exclusive_sibling_and_never_partial_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            record = self._run(root, free_bytes=0)
            path = write_invalid_preflight_record(record, repo_root=root)
            self.assertEqual(
                path,
                root / "results/spec107-c1-performance-r1.invalid-preflight.json",
            )
            self.assertFalse((root / "results/spec107-c1-performance-r1").exists())
            written = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(written["verdict"], "INVALID_PREFLIGHT")
            self.assertFalse(written["roleStartAllowed"])
            with self.assertRaisesRegex(PreflightError, "PREFLIGHT_RECORD_EXISTS"):
                write_invalid_preflight_record(record, repo_root=root)

    def test_pass_record_cannot_be_written_as_invalid_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            record = self._run(root)
            with self.assertRaisesRegex(PreflightError, "PREFLIGHT_RECORD_NOT_INVALID"):
                write_invalid_preflight_record(record, repo_root=root)

    def test_pass_claims_exactly_one_active_writer_before_output_creation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            record = self._run(root)
            marker = claim_campaign_writer(
                record, repo_root=root, pid=os.getpid())
            self.assertEqual(
                marker,
                root / "results/spec107-c1-performance-r1.writer.json")
            self.assertFalse(
                (root / "results/spec107-c1-performance-r1").exists())
            claim = json.loads(marker.read_text(encoding="utf-8"))
            self.assertEqual(claim["candidateId"], record["candidateId"])
            self.assertEqual(claim["campaignId"], record["campaignId"])
            self.assertEqual(claim["pid"], os.getpid())
            self.assertEqual(claim["state"], "ACTIVE")
            with self.assertRaisesRegex(
                    PreflightError, "PREFLIGHT_WRITER_EXISTS"):
                claim_campaign_writer(record, repo_root=root, pid=os.getpid())
            repeated = self._run(root)
            self.assertEqual(repeated["verdict"], "INVALID_PREFLIGHT")
            self.assertIn("OUTPUT_ACTIVE_WRITER", repeated["reasons"])

    def test_retained_invalid_preflight_is_terminal_when_conditions_recover(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            invalid = self._run(root, free_bytes=0)
            write_invalid_preflight_record(invalid, repo_root=root)
            recovered = self._run(root, free_bytes=8192)
            self.assertEqual(recovered["verdict"], "INVALID_PREFLIGHT")
            self.assertFalse(recovered["roleStartAllowed"])
            self.assertIn(
                "INVALID_PREFLIGHT_RECORD_EXISTS", recovered["reasons"])


if __name__ == "__main__":
    unittest.main()
