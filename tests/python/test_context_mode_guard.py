from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
GUARD_PATH = ROOT / "scripts" / "context_mode_guard.py"
SPEC = importlib.util.spec_from_file_location("ndnsf_context_mode_guard", GUARD_PATH)
assert SPEC is not None and SPEC.loader is not None
GUARD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GUARD
SPEC.loader.exec_module(GUARD)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def add_source(connection: sqlite3.Connection, label: str, path: Path,
               content_hash: str | None = None) -> None:
    connection.execute(
        "INSERT INTO sources(label, file_path, content_hash) VALUES (?, ?, ?)",
        (label, str(path), content_hash if content_hash is not None else digest(path)),
    )


class ContextModeGuardTest(unittest.TestCase):
    def _project(self, root: Path) -> tuple[Path, Path, str]:
        (root / ".specify" / "memory").mkdir(parents=True)
        feature = root / "specs" / "179-request-scoped-confidentiality"
        feature.mkdir(parents=True)
        pointer = root / ".specify" / "feature.json"
        pointer.write_text(json.dumps({
            "feature_directory": "specs/179-request-scoped-confidentiality",
        }), encoding="utf-8")
        guide = root / ".specify" / "memory" / "context-mode.md"
        guide.write_text("guide\n", encoding="utf-8")
        anchor = root / ".specify" / "memory" / "context-mode-project.md"
        anchor.write_text("anchor\n", encoding="utf-8")
        for filename in ("spec.md", "plan.md", "tasks.md"):
            (feature / filename).write_text(f"{filename}\n", encoding="utf-8")
        db = root / "content.db"
        with sqlite3.connect(db) as connection:
            connection.execute(
                "CREATE TABLE sources(label TEXT, file_path TEXT, content_hash TEXT)"
            )
            add_source(connection, "NDNSF active feature pointer", pointer)
            add_source(connection, "NDNSF Context Mode operating guide", guide)
            prefix = "NDNSF active feature 179-request-scoped-confidentiality:"
            for filename in ("spec.md", "plan.md", "tasks.md"):
                path = feature / filename
                add_source(connection, prefix + str(path), path)
            # Historical source rows remain in the shared ContentDB.  This row
            # is intentionally stale and must not poison active-feature health.
            old = root / "specs" / "170-old" / "tasks.md"
            old.parent.mkdir()
            old.write_text("changed\n", encoding="utf-8")
            add_source(connection, "NDNSF Spec 170-old:" + str(old), old, "stale")
            connection.commit()
        return feature, db, prefix

    def test_historical_stale_rows_do_not_block_active_sources(self) -> None:
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as directory:
            root = Path(directory)
            feature, db, _ = self._project(root)
            context = GUARD.load_feature_context(root)
            self.assertEqual(context.feature_path, feature)
            checks = GUARD.verify_file_backed_sources(context, db)
            self.assertEqual(len(checks), 5)

    def test_stale_active_row_still_fails_closed(self) -> None:
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as directory:
            root = Path(directory)
            _, db, prefix = self._project(root)
            path = root / "specs" / "179-request-scoped-confidentiality" / "tasks.md"
            with sqlite3.connect(db) as connection:
                connection.execute(
                    "UPDATE sources SET content_hash = ? WHERE label = ?",
                    ("stale", prefix + str(path)),
                )
                connection.commit()
            context = GUARD.load_feature_context(root)
            with self.assertRaises(GUARD.GuardError) as raised:
                GUARD.verify_file_backed_sources(context, db)
            self.assertEqual(raised.exception.code, GUARD.EXIT_SOURCE_MISMATCH)

    def _claude_settings(self, root: Path, platform: str = "claude-code") -> Path:
        home = root / ".claude"
        home.mkdir(parents=True)
        hook_root = "/opt/context-mode/hooks"
        hooks = {
            group: [{"hooks": [{"type": "command", "command": f"{hook_root}/{name}.mjs"}]}]
            for group, name in {
                "PreToolUse": "pretooluse",
                "PostToolUse": "posttooluse",
                "SessionStart": "sessionstart",
                "PreCompact": "precompact",
                "UserPromptSubmit": "userpromptsubmit",
                "Stop": "stop",
            }.items()
        }
        hooks["PreToolUse"].append({
            "matcher": "ctx_search|mcp__",
            "hooks": [{
                "type": "command",
                "command": f"python3 {GUARD_PATH} hook --platform claude-code",
            }],
        })
        (home / "settings.json").write_text(json.dumps({
            "env": {"CONTEXT_MODE_PLATFORM": platform},
            "hooks": hooks,
        }), encoding="utf-8")
        (root / ".claude.json").write_text(json.dumps({
            "mcpServers": {
                "context-mode": {
                    "type": "stdio",
                    "command": sys.executable,
                    "args": [],
                    "env": {"CONTEXT_MODE_PLATFORM": "claude-code"},
                },
                "codegraph": {
                    "type": "stdio",
                    "command": sys.executable,
                    "args": ["--no-watch"],
                },
            }
        }), encoding="utf-8")
        return home

    def test_claude_configuration_requires_canonical_platform_and_mcp(self) -> None:
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as directory:
            root = Path(directory)
            home = self._claude_settings(root)
            result = GUARD._verify_claude_configuration(
                home, time.time() + 100, GUARD_PATH
            )
            self.assertEqual(result["platform"], "claude-code")
            self.assertEqual(result["mcp_servers"], ["codegraph", "context-mode"])

            (home / "settings.json").write_text(
                (home / "settings.json").read_text(encoding="utf-8").replace(
                    '"claude-code"', '"claude"', 1
                ),
                encoding="utf-8",
            )
            with self.assertRaises(GUARD.GuardError) as raised:
                GUARD._verify_claude_configuration(
                    home, time.time() + 100, GUARD_PATH
                )
            self.assertIn("claude-code", str(raised.exception))

    def test_codex_probe_uses_strict_nullable_client_title(self) -> None:
        params = GUARD._codex_initialize_params()
        self.assertIsNone(params["clientInfo"]["title"])
        self.assertEqual(params["clientInfo"]["name"],
                         "ndnsf-context-mode-guard")
        self.assertTrue(params["capabilities"]["experimentalApi"])


if __name__ == "__main__":
    unittest.main()
