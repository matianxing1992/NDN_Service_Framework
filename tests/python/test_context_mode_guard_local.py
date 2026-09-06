from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "context_mode_guard.py"
SPEC = importlib.util.spec_from_file_location("context_mode_guard", MODULE_PATH)
assert SPEC and SPEC.loader
guard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = guard
SPEC.loader.exec_module(guard)


class ContextModeQueryGuardTest(unittest.TestCase):
    def test_generic_mixed_timeline_query_is_rejected(self):
        with self.assertRaises(guard.GuardError) as caught:
            guard.build_search_request(
                lane="session",
                queries=[
                    "active feature latest checkpoint unresolved blockers "
                    "rejected approaches"
                ],
                source="user-prompt",
                required=[],
            )
        self.assertEqual(caught.exception.code, guard.EXIT_INVALID_QUERY)

    def test_authority_query_is_relevance_ordered_and_exact_source(self):
        source = "NDNSF active feature 163-di-collaboration-planning:/repo/plan.md"
        request = guard.build_search_request(
            lane="authority",
            queries=[
                "GenericSelectionTxnStore first-terminal-wins "
                "163-di-collaboration-planning"
            ],
            source=source,
            required=["GenericSelectionTxnStore", "163-di-collaboration-planning"],
            known_sources={source},
        )
        self.assertEqual(request["ctx_search"]["sort"], "relevance")
        self.assertEqual(request["ctx_search"]["source"], source)

    def test_project_query_is_relevance_ordered_and_exact_anchor(self):
        source = "NDNSF project context anchor"
        request = guard.build_search_request(
            lane="project",
            queries=["cross-Spec architecture context anchor"],
            source=source,
            required=["cross-Spec"],
            known_sources={source},
        )
        self.assertEqual(request["lane"], "project")
        self.assertEqual(request["ctx_search"]["sort"], "relevance")
        self.assertEqual(request["ctx_search"]["source"], source)

    def test_auto_memory_result_is_rejected(self):
        with self.assertRaises(guard.GuardError) as caught:
            guard.validate_search_results(
                "[auto-memory | 2026-07-10 | project/AGENTS.md]\n"
                "GenericSelectionTxnStore",
                lane="authority",
                source="safe-source",
                required=["GenericSelectionTxnStore"],
            )
        self.assertEqual(caught.exception.code, guard.EXIT_CONTAMINATED_RESULT)

    def test_missing_exact_source_is_rejected(self):
        with self.assertRaises(guard.GuardError) as caught:
            guard.validate_search_results(
                "GenericSelectionTxnStore from some other source",
                lane="authority",
                source="safe-source",
                required=["GenericSelectionTxnStore"],
            )
        self.assertEqual(caught.exception.code, guard.EXIT_SOURCE_MISMATCH)

    def test_only_unambiguous_directory_base_becomes_a_source_selector(self):
        base = "NDNSF active feature 163-di-collaboration-planning"
        labels = {
            base + ":/repo/spec.md",
            base + ":/repo/plan.md",
            "prefix " + base + " collision",
        }
        selectors = guard.exact_source_selectors(labels)
        self.assertNotIn(base, selectors)
        selectors = guard.exact_source_selectors(
            {base + ":/repo/spec.md", base + ":/repo/plan.md"}
        )
        self.assertIn(base, selectors)


class ContextModeHookGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = (Path(self.temp.name) / "repo").resolve()
        self.other = (Path(self.temp.name) / "other").resolve()
        self.context_home = Path(self.temp.name) / "context-mode"
        (self.root / ".specify" / "memory").mkdir(parents=True)
        feature = self.root / "specs" / "163-di-collaboration-planning"
        feature.mkdir(parents=True)
        self.other.mkdir()
        (self.root / ".specify" / "feature.json").write_text(
            '{"feature_directory":"specs/163-di-collaboration-planning"}',
            encoding="utf-8",
        )
        (self.root / ".specify" / "memory" / "context-mode-project.md").write_text(
            "project anchor", encoding="utf-8"
        )
        (self.root / ".specify" / "memory" / "context-mode.md").write_text(
            "guide", encoding="utf-8"
        )
        for filename in ("spec.md", "plan.md", "tasks.md"):
            (feature / filename).write_text(filename, encoding="utf-8")
        content_dir = self.context_home / "content"
        content_dir.mkdir(parents=True)
        self.source = "NDNSF active feature pointer"
        with sqlite3.connect(content_dir / "project.db") as connection:
            connection.execute(
                "CREATE TABLE sources ("
                "label TEXT, file_path TEXT, content_hash TEXT)"
            )
            def add_source(label, path):
                connection.execute(
                    "INSERT INTO sources(label,file_path,content_hash) VALUES(?,?,?)",
                    (
                        label,
                        str(path.resolve()),
                        hashlib.sha256(path.read_bytes()).hexdigest(),
                    ),
                )

            add_source(
                self.source,
                self.root / ".specify" / "feature.json",
            )
            add_source(
                "NDNSF project context anchor",
                self.root / ".specify" / "memory" / "context-mode-project.md",
            )
            add_source(
                "NDNSF Context Mode operating guide",
                self.root / ".specify" / "memory" / "context-mode.md",
            )
            self.directory_source = (
                "NDNSF active feature 163-di-collaboration-planning"
            )
            for filename in ("spec.md", "plan.md"):
                add_source(
                    self.directory_source + f":{feature.resolve() / filename}",
                    feature / filename,
                )
            add_source(
                self.directory_source + f":{feature.resolve() / 'tasks.md'}",
                feature / "tasks.md",
            )

    def _event(self, tool_input, *, cwd=None, tool_name=None):
        return {
            "cwd": str(cwd or self.root),
            "tool_name": tool_name or "mcp__context_mode__ctx_search",
            "tool_input": tool_input,
        }

    def _evaluate(self, event):
        return guard.evaluate_pretool_hook(
            event, project_root=self.root, context_home=self.context_home
        )

    def test_other_repository_is_not_constrained(self):
        result = self._evaluate(
            self._event(
                {"queries": ["latest checkpoint"], "sort": "timeline"},
                cwd=self.other,
            )
        )
        self.assertEqual(result, {})

    def test_non_context_search_tool_is_not_constrained(self):
        result = self._evaluate(
            self._event(
                {"queries": ["latest checkpoint"], "sort": "timeline"},
                tool_name="mcp__another_server__search",
            )
        )
        self.assertEqual(result, {})

    def test_generic_mixed_timeline_is_denied(self):
        result = self._evaluate(
            self._event(
                {
                    "queries": [
                        "active feature latest checkpoint unresolved blockers"
                    ],
                    "source": "user-prompt",
                    "sort": "timeline",
                }
            )
        )
        self.assertEqual(
            result["hookSpecificOutput"]["permissionDecision"], "deny"
        )

    def test_exact_real_host_marker_timeline_is_allowed(self):
        result = self._evaluate(
            self._event(
                {
                    "queries": ["CTX_HOST_ACCEPT_20300101_A1B2C3"],
                    "sort": "timeline",
                }
            )
        )
        self.assertEqual(result, {})

    def test_explicit_session_source_and_identifiers_are_allowed(self):
        result = self._evaluate(
            self._event(
                {
                    "queries": [
                        "GenericSelectionTxnStore "
                        "163-di-collaboration-planning"
                    ],
                    "source": "decision",
                    "sort": "timeline",
                }
            )
        )
        self.assertEqual(result, {})

    def test_authority_without_exact_source_is_denied(self):
        result = self._evaluate(
            self._event(
                {
                    "queries": [
                        "latest checkpoint 163-di-collaboration-planning"
                    ],
                    "sort": "relevance",
                }
            )
        )
        self.assertEqual(
            result["hookSpecificOutput"]["permissionDecision"], "deny"
        )

    def test_authority_with_exact_source_and_identifier_is_allowed(self):
        result = self._evaluate(
            self._event(
                {
                    "queries": [
                        "latest checkpoint 163-di-collaboration-planning"
                    ],
                    "source": self.source,
                    "sort": "relevance",
                }
            )
        )
        self.assertEqual(result, {})

    def test_authority_accepts_directory_base_but_not_partial_label(self):
        safe = self._evaluate(
            self._event(
                {
                    "queries": ["GenericSelectionTxnStore"],
                    "source": self.directory_source,
                    "sort": "relevance",
                }
            )
        )
        self.assertEqual(safe, {})
        denied = self._evaluate(
            self._event(
                {
                    "queries": ["GenericSelectionTxnStore"],
                    "source": "NDNSF active feature",
                    "sort": "relevance",
                }
            )
        )
        self.assertEqual(
            denied["hookSpecificOutput"]["permissionDecision"], "deny"
        )

    def test_project_anchor_survives_active_spec_pointer_change(self):
        database = guard.find_project_content_db(self.root, self.context_home)
        self.assertTrue(database.is_file())
        (self.root / ".specify" / "feature.json").write_text(
            '{"feature_directory":"specs/164-other-feature"}',
            encoding="utf-8",
        )
        self.assertEqual(
            guard.find_project_content_db(self.root, self.context_home), database
        )


class ContextModeIndexGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.feature = self.root / "specs" / "163-di-collaboration-planning"
        (self.root / ".specify" / "memory").mkdir(parents=True)
        self.feature.mkdir(parents=True)
        (self.root / ".specify" / "feature.json").write_text(
            '{"feature_directory":"specs/163-di-collaboration-planning"}',
            encoding="utf-8",
        )
        (self.root / ".specify" / "memory" / "context-mode.md").write_text(
            "guide", encoding="utf-8"
        )
        for filename in ("spec.md", "plan.md", "tasks.md"):
            (self.feature / filename).write_text(filename, encoding="utf-8")
        self.database = Path(self.temp.name) / "content.db"
        with sqlite3.connect(self.database) as connection:
            connection.execute(
                "CREATE TABLE sources ("
                "id INTEGER PRIMARY KEY, label TEXT, file_path TEXT, content_hash TEXT)"
            )
            prefix = "NDNSF active feature 163-di-collaboration-planning:"
            sources = {
                "NDNSF active feature pointer": self.root
                / ".specify"
                / "feature.json",
                "NDNSF Context Mode operating guide": self.root
                / ".specify"
                / "memory"
                / "context-mode.md",
                **{
                    prefix + str(self.feature / filename): self.feature / filename
                    for filename in ("spec.md", "plan.md", "tasks.md")
                },
            }
            for label, path in sources.items():
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                connection.execute(
                    "INSERT INTO sources(label,file_path,content_hash) VALUES(?,?,?)",
                    (label, str(path.resolve()), digest),
                )

    def test_project_layer_sources_pass(self):
        anchor = self.root / ".specify" / "memory" / "context-mode-project.md"
        anchor.write_text("project anchor", encoding="utf-8")
        with sqlite3.connect(self.database) as connection:
            connection.execute(
                "INSERT INTO sources(label,file_path,content_hash) VALUES(?,?,?)",
                (
                    "NDNSF project context anchor",
                    str(anchor.resolve()),
                    hashlib.sha256(anchor.read_bytes()).hexdigest(),
                ),
            )
        checks = guard.verify_project_file_backed_sources(self.root, self.database)
        self.assertEqual(
            {check["label"] for check in checks},
            {
                "NDNSF project context anchor",
                "NDNSF Context Mode operating guide",
            },
        )

    def test_project_layer_does_not_require_active_feature_documents(self):
        anchor = self.root / ".specify" / "memory" / "context-mode-project.md"
        anchor.write_text("project anchor", encoding="utf-8")
        with sqlite3.connect(self.database) as connection:
            connection.execute(
                "INSERT INTO sources(label,file_path,content_hash) VALUES(?,?,?)",
                (
                    "NDNSF project context anchor",
                    str(anchor.resolve()),
                    hashlib.sha256(anchor.read_bytes()).hexdigest(),
                ),
            )
        for filename in ("spec.md", "plan.md", "tasks.md"):
            (self.feature / filename).unlink()
        checks = guard.verify_project_file_backed_sources(self.root, self.database)
        self.assertEqual(len(checks), 2)

    def test_fresh_file_backed_sources_pass(self):
        context = guard.load_feature_context(self.root)
        checks = guard.verify_file_backed_sources(context, self.database)
        self.assertEqual(len(checks), 5)

    def test_stale_file_backed_source_fails(self):
        (self.feature / "plan.md").write_text("changed", encoding="utf-8")
        context = guard.load_feature_context(self.root)
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_file_backed_sources(context, self.database)
        self.assertEqual(caught.exception.code, guard.EXIT_SOURCE_MISMATCH)
        self.assertIn("stale indexed source hash", str(caught.exception))


class ContextModeHostConfigurationGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.codex_home = Path(self.temp.name)
        (self.codex_home / "config.toml").write_text(
            """
            [features]
            hooks = true
            [mcp_servers.context-mode]
            command = "context-mode"
            [mcp_servers.context-mode.env]
            CONTEXT_MODE_PLATFORM = "codex"
            """,
            encoding="utf-8",
        )
        (self.codex_home / "hooks.json").write_text(
            json.dumps(
                {
                    "hooks": {
                        "SessionStart": [],
                        "UserPromptSubmit": [],
                        "PreCompact": [],
                        "PreToolUse": [
                            {
                                "hooks": [
                                    {
                                        "type": "command",
                                        "command": (
                                            "python3 "
                                            f"{guard.PROJECT_ROOT / 'scripts' / 'context_mode_guard.py'} "
                                            "hook"
                                        ),
                                    }
                                ]
                            }
                        ],
                    }
                }
            ),
            encoding="utf-8",
        )
        for path in self.codex_home.iterdir():
            os.utime(path, (100.0, 100.0))
        entries = [
            {
                "eventName": event,
                "command": f"context-mode hook codex {command}",
                "key": f"hooks.json:{event}",
                "trustStatus": "trusted",
                "enabled": True,
            }
            for event, command in guard.CODEX_CONTEXT_HOOKS
        ]
        entries.append(
            {
                "eventName": "preToolUse",
                "command": (
                    "python3 "
                    f"{guard.PROJECT_ROOT / 'scripts' / 'context_mode_guard.py'} "
                    "hook"
                ),
                "key": "hooks.json:guard",
                "trustStatus": "trusted",
                "enabled": True,
            }
        )
        patcher = mock.patch.object(
            guard, "_load_codex_hook_entries", return_value=entries
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_app_server_started_after_spaced_toml_configuration_passes(self):
        result = guard.verify_host_configuration(self.codex_home, app_start=200.0)
        self.assertTrue(result["loaded_after_configuration"])

    def test_removed_plugin_hooks_does_not_replace_stable_hooks(self):
        config = self.codex_home / "config.toml"
        config.write_text(
            config.read_text(encoding="utf-8").replace(
                "hooks = true", "plugin_hooks = true"
            ),
            encoding="utf-8",
        )
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(self.codex_home, app_start=200.0)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("hooks=true", str(caught.exception))

    def test_app_server_predating_hooks_json_requires_restart(self):
        os.utime(self.codex_home / "config.toml", (10.0, 10.0))
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(self.codex_home, app_start=50.0)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("HOST_RESTART_REQUIRED", str(caught.exception))
        self.assertIn("hooks.json", str(caught.exception))

    def test_runtime_config_toml_rewrite_does_not_require_restart(self):
        os.utime(self.codex_home / "config.toml", (300.0, 300.0))
        result = guard.verify_host_configuration(
            self.codex_home, app_start=200.0
        )
        self.assertTrue(result["loaded_after_configuration"])
        self.assertEqual(result["config_toml_mtime"], 300.0)
        self.assertEqual(result["hooks_json_mtime"], 100.0)
        self.assertFalse(result["config_toml_mtime_requires_restart"])

    def test_untrusted_context_mode_hook_fails_health(self):
        entries = [
            dict(entry) for entry in guard._load_codex_hook_entries(self.codex_home)
        ]
        entries[0] = {**entries[0], "trustStatus": "untrusted"}
        with mock.patch.object(
            guard, "_load_codex_hook_entries", return_value=entries
        ):
            with self.assertRaises(guard.GuardError) as caught:
                guard.verify_host_configuration(self.codex_home, app_start=200.0)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("HOOK_TRUST_REQUIRED", str(caught.exception))
        self.assertIn("untrusted Codex hook", str(caught.exception))

    def test_missing_query_guard_hook_fails_health(self):
        hooks = self.codex_home / "hooks.json"
        hooks.write_text(
            json.dumps(
                {
                    "hooks": {
                        "SessionStart": [],
                        "UserPromptSubmit": [],
                        "PreCompact": [],
                    }
                }
            ),
            encoding="utf-8",
        )
        os.utime(hooks, (100.0, 100.0))
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(self.codex_home, app_start=200.0)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("does not enforce", str(caught.exception))

    def test_guard_outside_pretooluse_does_not_count_as_enforcement(self):
        hooks = self.codex_home / "hooks.json"
        hooks.write_text(
            json.dumps(
                {
                    "hooks": {
                        "SessionStart": [],
                        "UserPromptSubmit": [],
                        "PreCompact": [],
                        "PostToolUse": [
                            {
                                "hooks": [
                                    {
                                        "command": (
                                            "python3 "
                                            f"{guard.PROJECT_ROOT / 'scripts' / 'context_mode_guard.py'} "
                                            "hook"
                                        )
                                    }
                                ]
                            }
                        ],
                    }
                }
            ),
            encoding="utf-8",
        )
        os.utime(hooks, (100.0, 100.0))
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(self.codex_home, app_start=200.0)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("does not enforce", str(caught.exception))


class ContextModeClaudeConfigurationGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.client_home = Path(self.temp.name) / "claude"
        self.client_home.mkdir()
        hook_commands = {
            "PreToolUse": "pretooluse",
            "PostToolUse": "posttooluse",
            "SessionStart": "sessionstart",
            "PreCompact": "precompact",
            "UserPromptSubmit": "userpromptsubmit",
            "Stop": "stop",
        }
        hooks = {
            group: [
                {
                    "matcher": "",
                    "hooks": [
                        {
                            "type": "command",
                            "command": (
                                "node /opt/context-mode/hooks/"
                                f"{command_name}.mjs"
                            ),
                        }
                    ],
                }
            ]
            for group, command_name in hook_commands.items()
        }
        hooks["PreToolUse"].append(
            {
                "matcher": "mcp__context_mode__ctx_search",
                "hooks": [
                    {
                        "type": "command",
                        "command": (
                            "python3 "
                            f"{guard.PROJECT_ROOT / 'scripts' / 'context_mode_guard.py'} "
                            "hook --platform claude-code"
                        ),
                    }
                ],
            }
        )
        self.settings = self.client_home / "settings.json"
        self.payload = {
            "env": {"CONTEXT_MODE_PLATFORM": "claude-code"},
            "hooks": hooks,
        }
        self._write_settings()

        registry = self.client_home.parent / ".claude.json"
        registry.write_text(json.dumps({"mcpServers": {
            "context-mode": {"type": "stdio", "command": sys.executable,
                             "args": [],
                             "env": {"CONTEXT_MODE_PLATFORM": "claude-code"}},
            "codegraph": {"type": "stdio", "command": sys.executable,
                          "args": ["serve", "--mcp", "--no-watch"]},
        }}), encoding="utf-8")
        os.utime(registry, (100.0, 100.0))

    def _write_settings(self):
        self.settings.write_text(
            json.dumps(self.payload),
            encoding="utf-8",
        )
        os.utime(self.settings, (100.0, 100.0))

    def test_six_hooks_guard_and_fresh_process_pass_static_check(self):
        result = guard.verify_host_configuration(
            self.client_home,
            app_start=200.0,
            platform="claude-code",
        )
        self.assertEqual(result["platform"], "claude-code")
        self.assertTrue(result["loaded_after_configuration"])

    def test_missing_one_of_six_context_hooks_fails(self):
        del self.payload["hooks"]["Stop"]
        self._write_settings()
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(
                self.client_home,
                app_start=200.0,
                platform="claude-code",
            )
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("Stop", str(caught.exception))

    def test_guard_without_claude_platform_flag_fails(self):
        guard_rule = self.payload["hooks"]["PreToolUse"][-1]
        guard_rule["hooks"][0]["command"] = (
            "python3 "
            f"{guard.PROJECT_ROOT / 'scripts' / 'context_mode_guard.py'} hook"
        )
        self._write_settings()
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_host_configuration(
                self.client_home,
                app_start=200.0,
                platform="claude-code",
            )
        self.assertIn("--platform claude-code", str(caught.exception))

    def test_no_real_claude_process_is_a_hard_failure(self):
        empty_proc = Path(self.temp.name) / "proc"
        empty_proc.mkdir()
        with self.assertRaises(guard.GuardError) as caught:
            guard.newest_client_start("claude-code", empty_proc)
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("no running Claude Code", str(caught.exception))

    def test_platform_defaults_are_client_private(self):
        self.assertEqual(
            guard.default_context_home("codex"),
            Path.home() / ".codex" / "context-mode",
        )
        self.assertEqual(
            guard.default_context_home("claude-code"),
            Path.home() / ".claude" / "context-mode",
        )
        self.assertEqual(
            guard.default_client_home("claude-code"),
            Path.home() / ".claude",
        )

    def test_codex_home_alias_is_rejected_for_claude(self):
        with self.assertRaises(guard.GuardError) as caught:
            guard.run_health(
                project_root=Path("/not-read"),
                context_home=Path("/not-read"),
                codex_home=Path("/legacy-codex-home"),
                platform="claude-code",
            )
        self.assertEqual(caught.exception.code, guard.EXIT_INVALID_QUERY)
        self.assertIn("--client-home", str(caught.exception))


class ContextModeMarkerGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = (Path(self.temp.name) / "correct-project").resolve()
        self.root.mkdir()
        self.database = Path(self.temp.name) / "sessions.db"
        with sqlite3.connect(self.database) as connection:
            connection.executescript(
                """
                CREATE TABLE session_meta (
                  session_id TEXT PRIMARY KEY,
                  project_dir TEXT NOT NULL,
                  started_at TEXT,
                  last_event_at TEXT,
                  event_count INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE session_events (
                  id INTEGER PRIMARY KEY,
                  session_id TEXT NOT NULL,
                  type TEXT NOT NULL,
                  category TEXT NOT NULL,
                  data TEXT NOT NULL,
                  project_dir TEXT NOT NULL,
                  created_at TEXT NOT NULL
                );
                """
            )

    def _insert(
        self,
        *,
        marker: str,
        project: Path,
        category: str = "user-prompt",
        created_at: str = "2030-01-01 00:00:02",
    ) -> None:
        with sqlite3.connect(self.database) as connection:
            session = "session-" + hashlib.sha256(str(project).encode()).hexdigest()[:8]
            connection.execute(
                "INSERT OR REPLACE INTO session_meta"
                "(session_id,project_dir,started_at,event_count) VALUES(?,?,?,1)",
                (session, str(project), "2030-01-01 00:00:00"),
            )
            connection.execute(
                "INSERT INTO session_events"
                "(session_id,type,category,data,project_dir,created_at) "
                "VALUES(?,?,?,?,?,?)",
                (
                    session,
                    "user",
                    category,
                    json.dumps({"prompt": marker}),
                    str(project),
                    created_at,
                ),
            )
        future = datetime(2030, 1, 1, 0, 0, 3, tzinfo=timezone.utc).timestamp()
        os.utime(self.database, (future, future))

    def test_wrong_project_marker_fails_binding(self):
        marker = "CTX_FIXTURE_WRONG_PROJECT_001"
        wrong = (Path(self.temp.name) / "wrong-project").resolve()
        wrong.mkdir()
        self._insert(marker=marker, project=wrong)
        threshold = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_session_marker(
                session_db=self.database,
                project_root=self.root,
                marker=marker,
                expect="present",
                after=[threshold],
                allow_fixture_marker=True,
            )
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("wrong event project", str(caught.exception))

    def test_correct_project_user_prompt_marker_passes(self):
        marker = "CTX_FIXTURE_CORRECT_PROJECT_001"
        self._insert(marker=marker, project=self.root)
        threshold = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        result = guard.verify_session_marker(
            session_db=self.database,
            project_root=self.root,
            marker=marker,
            expect="present",
            after=[threshold],
            allow_fixture_marker=True,
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["events"], 1)

    def test_post_search_event_does_not_invalidate_real_prompt(self):
        marker = "CTX_FIXTURE_SEARCH_THEN_VERIFY_001"
        self._insert(marker=marker, project=self.root, category="mcp")
        self._insert(marker=marker, project=self.root, category="user-prompt")
        threshold = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        result = guard.verify_session_marker(
            session_db=self.database,
            project_root=self.root,
            marker=marker,
            expect="present",
            after=[threshold],
            allow_fixture_marker=True,
        )
        self.assertEqual(result["events"], 1)
        self.assertEqual(result["ignored_non_user_prompt_events"], 1)

    def test_non_user_prompt_marker_alone_does_not_prove_capture(self):
        marker = "CTX_FIXTURE_NON_PROMPT_ONLY_001"
        self._insert(marker=marker, project=self.root, category="mcp")
        threshold = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_session_marker(
                session_db=self.database,
                project_root=self.root,
                marker=marker,
                expect="present",
                after=[threshold],
                allow_fixture_marker=True,
            )
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("non-user-prompt", str(caught.exception))

    def test_existing_marker_fails_absence_preflight(self):
        marker = "CTX_FIXTURE_DUPLICATE_001"
        self._insert(marker=marker, project=self.root)
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_session_marker(
                session_db=self.database,
                project_root=self.root,
                marker=marker,
                expect="absent",
                allow_fixture_marker=True,
            )
        self.assertEqual(caught.exception.code, guard.EXIT_CONTAMINATED_RESULT)

    def test_cli_mode_rejects_fixture_marker(self):
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_session_marker(
                session_db=self.database,
                project_root=self.root,
                marker="CTX_FIXTURE_NOT_REAL_001",
                expect="absent",
            )
        self.assertEqual(caught.exception.code, guard.EXIT_INVALID_QUERY)

    def test_millisecond_epoch_is_normalized(self):
        seconds = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        self.assertEqual(guard._parse_timestamp(seconds * 1000), seconds)

    def test_health_rejects_fixture_only_user_prompt_capture(self):
        marker = "CTX_FIXTURE_HEALTH_FALSE_PASS_001"
        self._insert(marker=marker, project=self.root)
        client_start = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        with self.assertRaises(guard.GuardError) as caught:
            guard.verify_current_project_session(
                self.database, self.root, client_start
            )
        self.assertEqual(
            caught.exception.code, guard.EXIT_PROJECT_BINDING_UNPROVEN
        )
        self.assertIn("NO_REAL_SESSION_EVENTS", str(caught.exception))

    def test_health_accepts_non_fixture_user_prompt_capture(self):
        marker = "normal-real-user-prompt"
        self._insert(marker=marker, project=self.root)
        client_start = datetime(2030, 1, 1, tzinfo=timezone.utc).timestamp()
        result = guard.verify_current_project_session(
            self.database, self.root, client_start
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["post_restart_user_prompts"], 1)


if __name__ == "__main__":
    unittest.main()
