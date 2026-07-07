# Claude Code Instructions

Read and follow `AGENTS.md` first. It is the canonical project guidance for
NDNSF, including CodeGraph-first exploration, Spec Kit, GSD, DeepSeek delegation,
Academic Research Suite (ARS), MiniNDN-first validation, git safety, and the
completion bell.

Default to Chinese in conversation with the user. Use English only when the
user explicitly asks for English, when writing source-code identifiers, or when
editing English technical artifacts such as papers, slides, comments, commit
messages, and documentation sections that are already in English.

## Shared Codex Tooling

Claude Code on this machine is configured to reuse the existing Codex-local
tooling instead of creating a second copy:

- Skills: `~/.claude/skills -> ~/.codex/skills`
- GSD: `/home/tianxing/.codex/gsd-core`
- Context Mode: `/home/tianxing/.local/node-v22.23.1/bin/context-mode`
- CodeGraph MCP: `codegraph serve --mcp`
- Claude Code backend model: DeepSeek v4 Pro through DeepSeek's
  Anthropic-compatible endpoint
- Optional DeepSeek delegate helper: `tools/ai/deepseek_delegate.py`

## Claude Code DeepSeek Backend

Claude Code is configured through `~/.claude/settings.json` to send its main
model requests to DeepSeek directly:

```text
ANTHROPIC_BASE_URL=https://api.deepseek.com/anthropic
ANTHROPIC_MODEL=deepseek-v4-pro[1m]
ANTHROPIC_DEFAULT_OPUS_MODEL=deepseek-v4-pro[1m]
ANTHROPIC_DEFAULT_SONNET_MODEL=deepseek-v4-pro[1m]
ANTHROPIC_DEFAULT_HAIKU_MODEL=deepseek-v4-flash
CLAUDE_CODE_SUBAGENT_MODEL=deepseek-v4-flash
CLAUDE_CODE_EFFORT_LEVEL=max
```

The API token is stored only in the local Claude settings file and should never
be printed, committed, or copied into prompts.

## DeepSeek Delegation

Because Claude Code itself already uses DeepSeek v4 Pro as the backend, the
delegate helper is optional. Use it only when a separate, isolated draft or
second-pass review is useful:

```bash
python3 tools/ai/deepseek_delegate.py --model deepseek-v4-pro --mode plan --task "<short task>"
python3 tools/ai/deepseek_delegate.py --model deepseek-v4-pro --mode patch --task "<short task>"
python3 tools/ai/deepseek_delegate.py --model deepseek-v4-pro --mode review --task "<short review request>"
```

DeepSeek output is advisory only. Review any generated patch, apply only the
parts that fit the architecture, and run the relevant tests yourself.

Do not print or copy API keys, certificates, tokens, safebags, or other secrets
into chat, commits, or DeepSeek prompts. The key is read from the local
`DEEPSEEK_API_KEY_FILE` environment variable configured in Claude settings.

## Finish Protocol

After completing a task, summarize changed files, validation, and the next most
valuable step. Then run:

```bash
sudo -n timeout 1s speaker-test -t sine -f 880 >/tmp/ndnsf_bell.log 2>&1 || true
```
