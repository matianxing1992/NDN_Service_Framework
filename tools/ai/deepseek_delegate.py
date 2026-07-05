#!/usr/bin/env python3
"""Ask DeepSeek for an advisory implementation draft.

This helper is intentionally conservative:

* it never edits files;
* it reads the API key from an environment variable or a local key file;
* it rejects likely secret/certificate files as context by default;
* it can dry-run the request payload without contacting DeepSeek.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_BASE_URL = "https://api.deepseek.com"
DEFAULT_MODEL = "deepseek-v4-pro"
DEFAULT_KEY_FILE = Path.home() / ".config" / "ndnsf" / "deepseek_api_key"
DEFAULT_MAX_CONTEXT_BYTES = 20000

SENSITIVE_MARKERS = (
    "api_key",
    "apikey",
    "auth_token",
    "bootstrap-token",
    "bootstrap.tokens",
    "certificate",
    "credential",
    "identity",
    "private",
    "secret",
    "token",
)
SENSITIVE_SUFFIXES = (
    ".cert",
    ".key",
    ".p12",
    ".pem",
)


class DelegateError(RuntimeError):
    pass


def repo_root_from(path: Path) -> Path:
    path = path.resolve()
    for candidate in (path, *path.parents):
        if (candidate / ".git").exists():
            return candidate
    return path


def load_api_key(api_key_file: Path | None = None) -> str:
    env_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if env_key:
        return env_key

    file_from_env = os.environ.get("DEEPSEEK_API_KEY_FILE", "").strip()
    key_file = api_key_file or (Path(file_from_env).expanduser() if file_from_env else DEFAULT_KEY_FILE)
    if key_file.exists():
        key = key_file.read_text(encoding="utf-8").strip()
        if key:
            return key

    raise DelegateError(
        "DeepSeek API key not found. Set DEEPSEEK_API_KEY or create "
        f"{DEFAULT_KEY_FILE} with mode 600."
    )


def is_sensitive_context_path(path: Path) -> bool:
    lowered = str(path).lower()
    if path.suffix.lower() in SENSITIVE_SUFFIXES:
        return True
    return any(marker in lowered for marker in SENSITIVE_MARKERS)


def read_context_file(
    path: Path,
    *,
    repo_root: Path,
    max_bytes: int,
    allow_outside_repo: bool,
    allow_sensitive: bool,
) -> str:
    resolved = path.expanduser().resolve()
    if not resolved.exists():
        raise DelegateError(f"Context file does not exist: {path}")
    if not resolved.is_file():
        raise DelegateError(f"Context path is not a file: {path}")
    if not allow_outside_repo:
        try:
            resolved.relative_to(repo_root)
        except ValueError as exc:
            raise DelegateError(
                f"Refusing context file outside repo: {path}. "
                "Pass --allow-outside-repo if this is intentional."
            ) from exc
    if not allow_sensitive and is_sensitive_context_path(resolved):
        raise DelegateError(
            f"Refusing likely sensitive context file: {path}. "
            "Pass --allow-sensitive-context only if you have reviewed it."
        )

    data = resolved.read_bytes()
    truncated = len(data) > max_bytes
    chunk = data[:max_bytes]
    try:
        text = chunk.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise DelegateError(f"Context file is not UTF-8 text: {path}") from exc

    rel = resolved
    try:
        rel = resolved.relative_to(repo_root)
    except ValueError:
        pass

    trailer = "\n[truncated]\n" if truncated else ""
    return f"### File: {rel}\n```text\n{text}{trailer}```"


def system_prompt(mode: str) -> str:
    base = (
        "You are DeepSeek acting as an advisory coding delegate for Codex. "
        "Do not claim that you changed files. Do not ask to run commands. "
        "Do not include secrets. Keep the output directly useful for Codex to "
        "review, edit, test, and commit."
    )
    if mode == "patch":
        return (
            base
            + " Return a concise implementation rationale followed by a unified diff. "
            "The diff is advisory only and must be minimal."
        )
    if mode == "plan":
        return base + " Return a concise design and task plan with risks and tests."
    if mode == "test":
        return base + " Return focused test cases and expected assertions."
    if mode == "review":
        return base + " Return review findings first, ordered by severity."
    raise DelegateError(f"Unsupported mode: {mode}")


def build_user_prompt(args: argparse.Namespace, repo_root: Path) -> str:
    parts = [
        "Task:",
        args.task.strip(),
        "",
        "Repository:",
        str(repo_root),
        "",
        "Constraints:",
        "- Treat your answer as advisory only.",
        "- Prefer small changes and existing project patterns.",
        "- Mention tests that should be run.",
    ]
    for text in args.context_text or []:
        parts.extend(["", "Additional context:", text])
    for filename in args.context_file or []:
        parts.extend(
            [
                "",
                read_context_file(
                    Path(filename),
                    repo_root=repo_root,
                    max_bytes=args.max_context_bytes,
                    allow_outside_repo=args.allow_outside_repo,
                    allow_sensitive=args.allow_sensitive_context,
                ),
            ]
        )
    return "\n".join(parts)


def build_payload(args: argparse.Namespace, repo_root: Path) -> dict[str, Any]:
    return {
        "model": args.model,
        "messages": [
            {"role": "system", "content": system_prompt(args.mode)},
            {"role": "user", "content": build_user_prompt(args, repo_root)},
        ],
        "temperature": args.temperature,
        "max_tokens": args.max_tokens,
        "stream": False,
    }


def call_deepseek(payload: dict[str, Any], *, base_url: str, api_key: str, timeout: float) -> dict[str, Any]:
    url = base_url.rstrip("/") + "/chat/completions"
    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise DelegateError(f"DeepSeek HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise DelegateError(f"DeepSeek request failed: {exc.reason}") from exc


def extract_content(response: dict[str, Any]) -> str:
    choices = response.get("choices") or []
    if not choices:
        raise DelegateError("DeepSeek response had no choices")
    message = choices[0].get("message") or {}
    content = message.get("content")
    if not isinstance(content, str) or not content.strip():
        raise DelegateError("DeepSeek response had empty content")
    return content


def write_or_print(text: str, output: str | None) -> None:
    if output:
        Path(output).expanduser().write_text(text, encoding="utf-8")
    else:
        print(text)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Ask DeepSeek for an advisory coding draft without editing files."
    )
    parser.add_argument("--task", required=True, help="Implementation/review task for DeepSeek.")
    parser.add_argument("--mode", choices=("patch", "plan", "test", "review"), default="patch")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--api-key-file", type=Path)
    parser.add_argument("--context-file", action="append", default=[])
    parser.add_argument("--context-text", action="append", default=[])
    parser.add_argument("--max-context-bytes", type=int, default=DEFAULT_MAX_CONTEXT_BYTES)
    parser.add_argument("--allow-outside-repo", action="store_true")
    parser.add_argument("--allow-sensitive-context", action="store_true")
    parser.add_argument("--temperature", type=float, default=0.2)
    parser.add_argument("--max-tokens", type=int, default=4096)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--output", help="Write DeepSeek response text to this file.")
    parser.add_argument("--dry-run", action="store_true", help="Print request payload without network.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    repo_root = repo_root_from(Path.cwd())
    try:
        payload = build_payload(args, repo_root)
        if args.dry_run:
            print(json.dumps({"base_url": args.base_url, "payload": payload}, indent=2))
            return 0

        api_key = load_api_key(args.api_key_file)
        response = call_deepseek(
            payload,
            base_url=args.base_url,
            api_key=api_key,
            timeout=args.timeout,
        )
        write_or_print(extract_content(response), args.output)
        return 0
    except DelegateError as exc:
        print(f"deepseek_delegate: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
