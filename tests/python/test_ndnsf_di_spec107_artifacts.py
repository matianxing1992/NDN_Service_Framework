#!/usr/bin/env python3
"""Spec 107 content-addressed artifact materialization tests."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from spec107_artifacts import (  # noqa: E402
    ArtifactError,
    materialize_artifact_set,
    verify_artifact_set,
)
from spec107_identity import build_candidate_identity  # noqa: E402


def _digest(character: str) -> str:
    return "sha256:" + character * 64


def candidate() -> dict[str, object]:
    return build_candidate_identity({
        "source": _digest("1"), "profile": _digest("2"),
        "model": _digest("3"), "plan": _digest("4"),
        "artifact": _digest("5"), "lineage": _digest("6"),
        "workload": _digest("7"), "tokenizer": _digest("8"),
        "trustPolicy": _digest("9"), "command": _digest("a"),
    }, created_at="2026-07-12T00:00:00Z")


class Spec107ArtifactsTest(unittest.TestCase):
    def _sources(self, root: Path, *, readonly: bool) -> tuple[Path, list[dict[str, str]]]:
        source = root / "source"
        source.mkdir(parents=True)
        rows = []
        for index in range(3):
            path = source / f"stage-{index}.onnx"
            path.write_bytes((f"stage-{index}-payload").encode())
            if readonly:
                path.chmod(0o444)
            rows.append({
                "role": f"/LLM/Pipeline/Stage/{index}",
                "source": path.name,
            })
        return source, rows

    def test_readonly_sources_use_safe_hardlinks_and_seal_store(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=True)
            (source / "export-intermediate.pt").write_bytes(b"temporary")
            result = materialize_artifact_set(
                source_root=source,
                output_root=root / "results/spec107-artifacts",
                artifacts=rows,
                candidate_id=candidate()["candidateId"],
                model_revision="Qwen/Qwen2.5-0.5B-Instruct@frozen",
                repo_root=root,
                reserve_bytes=0,
            )
            store = Path(result["storePath"])
            self.assertFalse((source / "export-intermediate.pt").exists())
            self.assertEqual(len(result["manifest"]["artifacts"]), 3)
            for index, row in enumerate(result["manifest"]["artifacts"]):
                destination = store / row["path"]
                self.assertEqual(row["storageMode"], "hardlink")
                self.assertEqual(destination.stat().st_ino,
                                 (source / f"stage-{index}.onnx").stat().st_ino)
                self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o444)
            self.assertEqual(stat.S_IMODE(store.stat().st_mode), 0o555)
            self.assertEqual(
                stat.S_IMODE((store / "artifact-set.json").stat().st_mode), 0o444)
            self.assertEqual(verify_artifact_set(store)["artifactSetDigest"],
                             result["manifest"]["artifactSetDigest"])

    def test_writable_sources_never_hardlink_and_copy_fallback_preserves_source_mode(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=False)
            with mock.patch("spec107_artifacts._try_reflink", return_value=False):
                result = materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows,
                    candidate_id=candidate()["candidateId"],
                    model_revision="frozen",
                    repo_root=root,
                    reserve_bytes=0,
                )
            store = Path(result["storePath"])
            for index, row in enumerate(result["manifest"]["artifacts"]):
                src = source / f"stage-{index}.onnx"
                dst = store / row["path"]
                self.assertEqual(row["storageMode"], "copy")
                self.assertNotEqual(src.stat().st_ino, dst.stat().st_ino)
                self.assertTrue(src.stat().st_mode & stat.S_IWUSR)
                self.assertFalse(dst.stat().st_mode & stat.S_IWUSR)

    def test_readonly_hardlink_failure_falls_back_to_verified_copy(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=True)
            with mock.patch("spec107_artifacts.os.link", side_effect=OSError("no link")), \
                    mock.patch("spec107_artifacts._try_reflink", return_value=False):
                result = materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows,
                    candidate_id=candidate()["candidateId"],
                    model_revision="frozen",
                    repo_root=root,
                    reserve_bytes=0,
                )
            self.assertEqual(
                {row["storageMode"] for row in result["manifest"]["artifacts"]},
                {"copy"},
            )

    def test_existing_verified_digest_store_is_reused_without_rewrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=True)
            first = materialize_artifact_set(
                source_root=source, output_root=root / "results/spec107-artifacts",
                artifacts=rows, candidate_id=candidate()["candidateId"],
                model_revision="frozen", repo_root=root, reserve_bytes=0)
            manifest_path = Path(first["storePath"]) / "artifact-set.json"
            inode = manifest_path.stat().st_ino
            second = materialize_artifact_set(
                source_root=source, output_root=root / "results/spec107-artifacts",
                artifacts=rows, candidate_id=candidate()["candidateId"],
                model_revision="frozen", repo_root=root, reserve_bytes=0)
            self.assertEqual(second["materialization"], "REUSED")
            self.assertEqual(manifest_path.stat().st_ino, inode)
            self.assertEqual(first["manifest"], second["manifest"])

    def test_tampered_existing_store_is_rejected_not_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=False)
            with mock.patch("spec107_artifacts._try_reflink", return_value=False):
                result = materialize_artifact_set(
                    source_root=source, output_root=root / "results/spec107-artifacts",
                    artifacts=rows, candidate_id=candidate()["candidateId"],
                    model_revision="frozen", repo_root=root, reserve_bytes=0)
            artifact = Path(result["storePath"]) / "stage-1.onnx"
            artifact.chmod(0o644)
            artifact.write_bytes(b"tampered")
            with self.assertRaisesRegex(ArtifactError, "ARTIFACT_SET_DIGEST_MISMATCH"):
                with mock.patch("spec107_artifacts._try_reflink", return_value=False):
                    materialize_artifact_set(
                        source_root=source,
                        output_root=root / "results/spec107-artifacts",
                        artifacts=rows, candidate_id=candidate()["candidateId"],
                        model_revision="frozen", repo_root=root, reserve_bytes=0)

    def test_source_escape_symlink_and_spec105_intermediate_mutation_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=True)
            outside = root / "outside.onnx"
            outside.write_bytes(b"outside")
            (source / "stage-1.onnx").unlink()
            (source / "stage-1.onnx").symlink_to(outside)
            with self.assertRaisesRegex(ArtifactError, "ARTIFACT_SOURCE_ESCAPE"):
                materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows, candidate_id=candidate()["candidateId"],
                    model_revision="frozen", repo_root=root, reserve_bytes=0)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "results/spec105-retained"
            source.mkdir(parents=True)
            rows = []
            for index in range(3):
                path = source / f"stage-{index}.onnx"
                path.write_bytes(b"x")
                path.chmod(0o444)
                rows.append({
                    "role": f"/LLM/Pipeline/Stage/{index}",
                    "source": path.name,
                })
            (source / "intermediate.pt").write_bytes(b"must remain")
            with self.assertRaisesRegex(ArtifactError, "SPEC105_INTERMEDIATE_MUTATION_DENIED"):
                materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows, candidate_id=candidate()["candidateId"],
                    model_revision="frozen", repo_root=root, reserve_bytes=0)
            self.assertTrue((source / "intermediate.pt").exists())

    def test_requires_exactly_three_distinct_roles_and_enough_free_space(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, rows = self._sources(root, readonly=False)
            with self.assertRaisesRegex(ArtifactError, "ARTIFACT_ROLE_SET_INVALID"):
                materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows[:2], candidate_id=candidate()["candidateId"],
                    model_revision="frozen", repo_root=root, reserve_bytes=0)
            with self.assertRaisesRegex(ArtifactError, "ARTIFACT_SPACE_INSUFFICIENT"):
                materialize_artifact_set(
                    source_root=source,
                    output_root=root / "results/spec107-artifacts",
                    artifacts=rows, candidate_id=candidate()["candidateId"],
                    model_revision="frozen", repo_root=root,
                    reserve_bytes=1024, free_bytes=1024)

    def test_cli_prepares_and_reuses_artifacts_without_candidate_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, _ = self._sources(root, readonly=False)
            model = root / "model.json"
            model.write_text(json.dumps({
                "model": {"id": "Qwen/Qwen2.5-0.5B-Instruct", "revision": "frozen"},
            }), encoding="utf-8")
            cli = REPO / "tools/ndnsf-di/spec107_candidate.py"
            command = [
                sys.executable, str(cli), "--repo-root", str(root),
                "artifact", "prepare", "--source", str(source),
                "--output-root", str(root / "results/spec107-artifacts"),
                "--model-manifest", str(model), "--reserve-bytes", "0",
            ]
            first = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            first_payload = json.loads(first.stdout)
            self.assertEqual(first_payload["materialization"], "CREATED")
            self.assertEqual(
                [row["role"] for row in first_payload["manifest"]["artifacts"]],
                [
                    "/LLM/Pipeline/Stage/0",
                    "/LLM/Pipeline/Stage/1",
                    "/LLM/Pipeline/Stage/2",
                ],
            )
            second = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(json.loads(second.stdout)["materialization"], "REUSED")


if __name__ == "__main__":
    unittest.main()
