#!/usr/bin/env python3
"""Tests for the read-only Occam source inventory."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "maintenance" / "ndnsf_occam_audit.py"
SPEC = importlib.util.spec_from_file_location("ndnsf_occam_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


class OccamAuditTest(unittest.TestCase):
    def make_tree(self, root: Path) -> None:
        files = {
            "src/runtime.py": "PublishRequest()\n",
            "tests/test_runtime.py": "PublishRequest()\n",
            "docs/migration.md": "PublishRequest\n",
            "specs/001-old/spec.md": "PublishRequest\n",
            "build/generated/runtime.cpp": "PublishRequest();\n",
            "examples/demo.py": "PublishRequest()\n",
            "third_party/vendor/runtime.cpp": "PublishRequest();\n",
            "Experiments/gRPC/vendor.cpp": "PublishRequest();\n",
            "src/clean.py": "def request_service():\n    pass\n",
        }
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

    def test_scan_classifies_active_test_docs_specs_generated_and_examples(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_tree(root)
            findings = AUDIT.scan(root, ["v1-invocation"])

        by_path = {item.path: item.classification for item in findings}
        self.assertEqual(by_path["src/runtime.py"], "active")
        self.assertEqual(by_path["tests/test_runtime.py"], "test")
        self.assertEqual(by_path["docs/migration.md"], "docs")
        self.assertEqual(by_path["specs/001-old/spec.md"], "historical-spec")
        self.assertEqual(by_path["build/generated/runtime.cpp"], "generated")
        self.assertEqual(by_path["examples/demo.py"], "example")
        self.assertNotIn("third_party/vendor/runtime.cpp", by_path)
        self.assertNotIn("Experiments/gRPC/vendor.cpp", by_path)
        self.assertNotIn("src/clean.py", by_path)

    def test_fail_on_active_uses_only_active_classification(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs").mkdir()
            (root / "docs" / "old.md").write_text("BloomFilter\n")
            clean = subprocess.run(
                [sys.executable, str(SCRIPT), str(root), "--rule", "v1-invocation",
                 "--json", "--fail-on-active"],
                check=False, capture_output=True, text=True)
            self.assertEqual(clean.returncode, 0)
            (root / "src").mkdir()
            (root / "src" / "old.cpp").write_text("BloomFilter value;\n")
            blocked = subprocess.run(
                [sys.executable, str(SCRIPT), str(root), "--rule", "v1-invocation",
                 "--json", "--fail-on-active"],
                check=False, capture_output=True, text=True)

        self.assertEqual(blocked.returncode, 1)
        payload = json.loads(blocked.stdout)
        self.assertEqual(payload["summary"]["active"], 1)

    def test_unknown_rule_is_rejected_by_library_api(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "unknown rules"):
                AUDIT.scan(Path(tmp), ["not-a-rule"])

    def test_app_owned_types_and_internal_repo_binding_are_not_core_leaks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "NDNSF-DistributedInference/plan.py": "ExecutionArtifactSpec()\n",
                "NDNSF-DistributedRepo/adapter.py": "RepoDataPlaneProducer()\n",
                "pythonWrapper/src/ndnsf/binding.cpp": "RepoDataPlaneProducer value;\n",
                "pythonWrapper/ndnsf/runtime.py": "CoordinationIntent value;\n",
            }
            for name, content in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            findings = AUDIT.scan(root, ["core-application-leakage"])

        self.assertEqual(
            [(item.path, item.text) for item in findings],
            [("pythonWrapper/ndnsf/runtime.py", "CoordinationIntent value;")],
        )

    def test_removed_di_and_repo_surfaces_are_actionable_rules(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "di.py").write_text(
                'mode = "process-pool"\nrepo_manifests = {}\n')
            (root / "src" / "repo.py").write_text(
                "InMemoryRepoStore()\nisolated_runtime = True\n")
            di = AUDIT.scan(root, ["obsolete-di-surface"])
            repo = AUDIT.scan(root, ["obsolete-repo-surface"])

        self.assertEqual(len(di), 2)
        self.assertTrue(all(item.classification == "active" for item in di))
        self.assertEqual(len(repo), 2)
        self.assertTrue(all(item.classification == "active" for item in repo))


if __name__ == "__main__":
    unittest.main()
