---
name: codegraph-first
description: Prefer CodeGraph for semantic codebase exploration before falling back to text search. Use when searching code, tracing symbols, inspecting call graphs, reviewing impact, auditing implementations, or answering code-structure questions in a repository with CodeGraph installed.
---

# CodeGraph-First Code Search

## Purpose

Use CodeGraph as the first pass for semantic code exploration. It is best for
finding symbols, owners, callers, callees, impact surfaces, and architecture
entry points without repeatedly scanning the whole repository.

Use `rg` after CodeGraph when the task needs exact text matches, log strings,
config keys, generated names, non-code files, or verification of a precise
snippet.

## Quick Start

Before broad code exploration:

```bash
codegraph status .
```

If the index is stale or missing:

```bash
codegraph sync .
```

Search a symbol:

```bash
codegraph query RequestServiceTargeted --path . --limit 10
```

Explore a concept:

```bash
codegraph explore "How does targeted request authentication work?" --path .
```

Trace relationships:

```bash
codegraph callers <symbol-or-node-id> --path .
codegraph callees <symbol-or-node-id> --path .
codegraph impact <symbol-or-node-id> --path .
```

## Workflow

1. Start with `codegraph status .` for repository-level tasks.
2. Use `codegraph query` for known symbol, class, method, or function names.
3. Use `codegraph explore` for architecture questions or unfamiliar features.
4. Use callers/callees/impact/affected when reviewing behavior changes.
5. Use `rg` to confirm exact text, configs, docs, logs, tests, and edge cases.
6. When CodeGraph and text search disagree, inspect the actual source files.

## Rules

- Do not rely only on CodeGraph for final bug claims; verify with source.
- Do not use CodeGraph for secrets, generated binary artifacts, or build output.
- Keep `.codegraph/` local; do not commit the index.
- If CodeGraph is unavailable, stale, or errors repeatedly, say so briefly and
  fall back to `rg`.
- For performance-sensitive investigation, prefer CodeGraph to find the path,
  then instrument or inspect only the relevant files.

## Good Fits

- "Where is this API implemented?"
- "Who calls this handler?"
- "What breaks if I change this class?"
- "Review this feature path."
- "Trace request/ACK/selection/response flow."
- "Find all providers of this service abstraction."

## Poor Fits

- Searching for a literal error line.
- Checking README wording.
- Finding shell flags in scripts.
- Looking for serialized field names in YAML/JSON.
- Inspecting unindexed temporary output.
