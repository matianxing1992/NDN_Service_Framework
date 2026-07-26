from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests/native-external-runner/runner.cpp"


class NativeExternalRunnerFixtureTest(unittest.TestCase):
    def test_public_header_fixture_compiles_without_model_dependencies(self):
        with tempfile.TemporaryDirectory() as output:
            completed = subprocess.run(
                ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT), "-c", str(SOURCE),
                 "-o", str(Path(output) / "external-runner.o")],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(completed.returncode, 0,
                             completed.stdout + completed.stderr)

    def test_missing_public_header_dependency_fails_closed(self):
        with tempfile.TemporaryDirectory() as output:
            completed = subprocess.run(
                ["g++", "-std=c++17", "-c", str(SOURCE),
                 "-o", str(Path(output) / "external-runner.o")],
                cwd=output, text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("NativeModelRunner.hpp", completed.stderr)


if __name__ == "__main__":
    unittest.main()
