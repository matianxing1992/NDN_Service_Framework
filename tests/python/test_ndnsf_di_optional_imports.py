from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/optional-imports.json"
HELPER = ROOT / "tests/python/support/ndnsf_di_import_snapshot.py"


class OptionalImportsTest(unittest.TestCase):
    def test_root_package_imports_without_optional_dependency_families(self) -> None:
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(fixture["schema"], "ndnsf-di-spec111-optional-imports-v1")
        env = dict(os.environ)
        env["PYTHONPATH"] = str(ROOT / "NDNSF-DistributedInference") + os.pathsep + env.get("PYTHONPATH", "")
        for case in fixture["cases"]:
            command = [sys.executable, str(HELPER), case["target"]]
            for blocked in case["blocked"]:
                command += ["--blocked", blocked]
            completed = subprocess.run(command, cwd=ROOT, env=env, text=True, capture_output=True, check=False)
            with self.subTest(case=case["id"]):
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                self.assertEqual(json.loads(completed.stdout)["forbiddenLoaded"], [])


if __name__ == "__main__":
    unittest.main()
