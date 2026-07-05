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

    def test_usage_summary_from_jsonl_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            usage_log = Path(tmp) / "usage.jsonl"
            usage_log.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-07-05T00:00:00+00:00",
                                "model": "deepseek-v4-pro",
                                "usage": {
                                    "prompt_tokens": 10,
                                    "completion_tokens": 5,
                                    "total_tokens": 15,
                                    "prompt_cache_hit_tokens": 3,
                                    "prompt_cache_miss_tokens": 7,
                                    "completion_tokens_details": {"reasoning_tokens": 2},
                                },
                            }
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-07-05T00:01:00+00:00",
                                "model": "deepseek-v4-flash",
                                "usage": {
                                    "prompt_tokens": 20,
                                    "completion_tokens": 8,
                                    "total_tokens": 28,
                                },
                            }
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "--usage-summary",
                    "--usage-json",
                    "--usage-log",
                    str(usage_log),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            payload = json.loads(proc.stdout)
            self.assertEqual(payload["calls"], 2)
            self.assertEqual(payload["prompt_tokens"], 30)
            self.assertEqual(payload["completion_tokens"], 13)
            self.assertEqual(payload["total_tokens"], 43)
            self.assertEqual(payload["prompt_cache_hit_tokens"], 3)
            self.assertEqual(payload["prompt_cache_miss_tokens"], 7)
            self.assertEqual(payload["reasoning_tokens"], 2)

    def test_call_path_records_usage_without_printing_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "probe.py"
            usage_log = Path(tmp) / "usage.jsonl"
            output = Path(tmp) / "answer.txt"
            probe.write_text(
                "import json, sys\n"
                f"sys.path.insert(0, {str(REPO)!r})\n"
                "from pathlib import Path\n"
                "from tools.ai import deepseek_delegate as d\n"
                "def fake_call(payload, *, base_url, api_key, timeout):\n"
                "    assert api_key == 'unit-secret'\n"
                "    return {\n"
                "        'model': payload['model'],\n"
                "        'choices': [{'message': {'content': 'delegate ok'}}],\n"
                "        'usage': {'prompt_tokens': 4, 'completion_tokens': 2, 'total_tokens': 6},\n"
                "    }\n"
                "d.call_deepseek = fake_call\n"
                "rc = d.main([\n"
                "    '--task', 'unit',\n"
                "    '--api-key-file', sys.argv[1],\n"
                "    '--usage-log', sys.argv[2],\n"
                "    '--output', sys.argv[3],\n"
                "])\n"
                "raise SystemExit(rc)\n",
                encoding="utf-8",
            )
            key_file = Path(tmp) / "key"
            key_file.write_text("unit-secret\n", encoding="utf-8")
            proc = subprocess.run(
                ["python3", str(probe), str(key_file), str(usage_log), str(output)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            self.assertEqual(output.read_text(encoding="utf-8"), "delegate ok")
            self.assertNotIn("unit-secret", proc.stdout + proc.stderr)
            records = [json.loads(line) for line in usage_log.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0]["usage"]["total_tokens"], 6)


if __name__ == "__main__":
    unittest.main()
