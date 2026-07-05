import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "ai" / "deepseek_delegate.py"


class DeepSeekDelegateTests(unittest.TestCase):
    def test_dry_run_does_not_print_api_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            key_file = Path(tmp) / "deepseek.key"
            key_file.write_text("test-secret-key\n", encoding="utf-8")
            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "--task",
                    "Draft a no-op patch.",
                    "--api-key-file",
                    str(key_file),
                    "--dry-run",
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            self.assertNotIn("test-secret-key", proc.stdout)
            payload = json.loads(proc.stdout)
            self.assertEqual(payload["payload"]["model"], "deepseek-v4-pro")

    def test_context_file_is_embedded_in_dry_run(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPO) as tmp:
            context = Path(tmp) / "context.txt"
            context.write_text("hello context\n", encoding="utf-8")
            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "--task",
                    "Use this context.",
                    "--context-file",
                    str(context),
                    "--dry-run",
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            self.assertIn("hello context", proc.stdout)

    def test_sensitive_context_is_rejected_by_default(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPO) as tmp:
            context = Path(tmp) / "bootstrap-token.txt"
            context.write_text("do-not-send\n", encoding="utf-8")
            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "--task",
                    "Use this context.",
                    "--context-file",
                    str(context),
                    "--dry-run",
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 2)
            self.assertIn("Refusing likely sensitive context file", proc.stderr)

    def test_env_key_takes_precedence_over_file_for_non_dry_run_unit_path(self) -> None:
        env = os.environ.copy()
        env["DEEPSEEK_API_KEY"] = "env-secret"
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "probe.py"
            probe.write_text(
                "import os, sys\n"
                f"sys.path.insert(0, {str(REPO)!r})\n"
                "from tools.ai.deepseek_delegate import load_api_key\n"
                "print(load_api_key())\n",
                encoding="utf-8",
            )
            proc = subprocess.run(
                ["python3", str(probe)],
                cwd=REPO,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            self.assertEqual(proc.stdout.strip(), "env-secret")


if __name__ == "__main__":
    unittest.main()
