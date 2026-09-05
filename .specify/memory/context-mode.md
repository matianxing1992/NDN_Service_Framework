# Context Mode Operating Guide

This file is the single canonical Context Mode guide for both Codex and Claude
Code in the NDNSF workspace. Do not maintain client-specific copies. Context
Mode accelerates retrieval; it does not replace Spec Kit, the source tree,
CodeGraph, Git state, or experiment evidence.

## Authority Order

When sources disagree, use this order:

1. current source and configuration verified with CodeGraph or direct
   inspection;
2. the stable project Context Mode layer in
   `.specify/memory/context-mode-project.md` and other exact, maintained
   cross-Spec sources;
3. active Spec Kit feature selected by `.specify/feature.json`;
4. `specs/<active-feature>/spec.md`, `specs/<active-feature>/plan.md`,
   `specs/<active-feature>/tasks.md`, and that feature's contracts, audits,
   traceability, and frozen evidence manifests;
5. `AGENTS.md` and the constitution;
6. GSD resume/handoff state;
7. Context Mode search results;
8. conversational recollection.

Context Mode output is cached/retrieved context, not evidence that code ran,
tasks completed, or a benchmark passed.

## Start-of-Task Procedure

Context Mode MCP tools may be deferred. If `ctx_stats` and `ctx_search` are not
visible, search for the `mcp__context_mode` tools before declaring the plugin
missing.

For substantial work:

1. Run `ctx_stats` only as an anomaly screen. Its totals do not prove that the
   current host dispatches hooks or that events belong to the current project.
2. Run the project-layer repository guard (the default, cross-Spec check):

   ```bash
   python3 scripts/context_mode_guard.py health --project-root .
   ```

   A nonzero exit makes Context Mode advisory-only for the task. Continue from
   the repository fallback; do not weaken or bypass the check. Before reading
   the current checkpoint, run the stricter active-Spec layer as well:

   ```bash
   python3 scripts/context_mode_guard.py health \
     --scope active --project-root .
   ```

3. Read `.specify/feature.json` before searching. Extract:

   - the exact feature-directory basename;
   - one or two feature-specific protocol, API, or artifact identifiers;
   - a distinctive non-sensitive phrase from the latest user correction.

4. Select exactly one retrieval lane and validate the proposed query with the
   guard before calling `ctx_search`.

   **Project lane** retrieves stable cross-Spec architecture, API, and
   historical design material. It uses an exact source label and relevance
   ordering, but does not claim that a result is the current checkpoint:

   ```bash
   python3 scripts/context_mode_guard.py query \
     --lane project \
     --source "NDNSF project context anchor" \
     --query "cross-Spec architecture context anchor" \
     --require cross-Spec
   ```

   **Authority lane** retrieves maintained artifacts for the active Spec:

   ```bash
   python3 scripts/context_mode_guard.py query \
     --lane authority \
     --source "NDNSF active feature 163-di-collaboration-planning" \
     --query "GenericSelectionTxnStore DISelectionAcceptanceV2" \
     --require GenericSelectionTxnStore \
     --require DISelectionAcceptanceV2
   ```

   Use the exact file-backed source label and `sort: "relevance"`. Read the
   active pointer first. Treat current `tasks.md`, a closure report, or a
   handoff as the latest verified checkpoint; timeline auto-memory is never
   checkpoint authority.

   **Session lane** recalls one session event:

   ```text
   queries:
     - <one exact unique marker or high-entropy event identifier>
   sort: timeline
   category: <one explicit session-event category>
   ```

   Use one intent and one unique identifier. Do not combine active feature,
   latest checkpoint, unresolved blockers, and rejected approaches in one
   timeline batch. Reject every AGENTS/CLAUDE auto-memory result.

5. Validate every returned result with the guard and by inspection:

   - it belongs to this repository;
   - it names the active feature;
   - it contains at least one requested high-entropy identifier;
   - it came from the requested file-backed source or session-event category;
   - it is newer than superseded decisions;
   - file-backed content is not flagged stale;
   - it is not an AGENTS/CLAUDE auto-memory fragment;
   - it does not contradict current source, tasks, or evidence.

6. Read the durable fallback:

   ```text
   AGENTS.md
   .specify/feature.json
   .specify/memory/constitution.md
   specs/<active-feature>/spec.md
   specs/<active-feature>/plan.md
   specs/<active-feature>/tasks.md
   relevant contracts, audits, traceability, and evidence manifests
   git status --short
   ```

7. Use CodeGraph before broad source exploration.

The task may continue when Context Mode is empty or stale as long as the
repository fallback is complete. Record the fallback in the final report for a
substantial task.

## Known v1.0.169 Timeline Contamination

Context Mode v1.0.169 does not treat timeline search as a strict query over
captured session events. Its timeline path:

1. live-scans project and client-level AGENTS/CLAUDE files as auto-memory
   candidates on every search;
2. accepts a candidate when any sufficiently long query term matches;
3. does not apply the requested `source` filter to those auto-memory
   candidates;
4. merges them with file-backed content and SessionDB events;
5. sorts the merged set oldest-first and only then applies `limit`.

Consequently, an old AGENTS or CLAUDE fragment can consume every result slot
even when the requested source points at the active Spec. A generic multi-topic
query makes this deterministic contamination more likely; it is not evidence
that the active source disappeared.

`SessionStart` may also persist a rule-file snapshot in SessionDB. Such a
snapshot is historical session data, not current project authority, and must
fail the same AGENTS/CLAUDE rejection rule.

Do not use `ctx_purge` to repair this condition. Purge can delete valid indexed
file-backed sources, while the next timeline search immediately reads the same
live AGENTS/CLAUDE files again. The safe response is:

- authority facts: pointer first, then relevance plus the exact file-backed
  source;
- session recall: one unique marker or high-entropy identifier, explicit
  category, and rejection of every auto-memory result;
- failure or ambiguity: repository fallback.

Do not hand-edit the installed Context Mode package as routine maintenance.
The only current exception is the documented v1.0.169 saturated-session
prompt-retention hotfix below. Revalidate it after every official upgrade and
remove the hotfix when an executable regression proves that the official
release preserves recent raw prompts at the event cap.

## Executable Guard

`scripts/context_mode_guard.py` is the fail-closed preflight and acceptance
tool. It does not replace `ctx_search`; it prevents known-unsafe query shapes
and verifies local source/session state.

The current workspace also registers its `hook` subcommand as a second
`PreToolUse` hook for `ctx_search`. The hook is project-scoped: it allows other
repositories unchanged, but denies unsafe Context Mode searches when the hook
event's `cwd` is this repository. `health` fails unless that hook is present,
so an official upgrade or manual configuration edit cannot silently remove the
enforcement layer. Instructions remain necessary for clients that do not
dispatch `PreToolUse`; their Context Mode results stay advisory-only.

Stable commands:

```bash
# Validate the stable project anchor, source hashes, Codex hook trust,
# host/config freshness, and project SessionDB.
python3 scripts/context_mode_guard.py health --project-root .

# Validate the active pointer and active-Spec source hashes before checkpoint
# or implementation-authority queries.
python3 scripts/context_mode_guard.py health --scope active --project-root .

# Validate a stable cross-Spec query before ctx_search.
python3 scripts/context_mode_guard.py query \
  --lane project \
  --source "NDNSF project context anchor" \
  --query "cross-Spec architecture context anchor" \
  --require cross-Spec

# Validate an authority-lane query before ctx_search.
python3 scripts/context_mode_guard.py query \
  --lane authority \
  --source "NDNSF active feature 163-di-collaboration-planning" \
  --query "GenericSelectionTxnStore DISelectionAcceptanceV2" \
  --require GenericSelectionTxnStore \
  --require DISelectionAcceptanceV2

# Validate a real host marker against this project's SessionDB.
python3 scripts/context_mode_guard.py marker \
  --value CTX_HOST_ACCEPT_<date>_<feature-id>_<nonce> \
  --expect present \
  --project-root .
```

The commands default to Codex. In Claude Code, add
`--platform claude-code` to `health`, `marker`, and `hook`; this selects
`~/.claude/context-mode`, validates `~/.claude/settings.json`, and compares
events with the real Claude client start time. A running Codex process cannot
satisfy Claude acceptance, or vice versa.

Exit status is part of the contract:

```text
0 = requested invariant is proven
2 = invalid or low-entropy query
3 = contaminated result
4 = source, category, or required-identifier mismatch
5 = current-project binding is not proven
```

Never convert a nonzero result to success in a wrapper. Test fixtures use only
`CTX_FIXTURE_*`; the CLI reserves `CTX_HOST_ACCEPT_*` for a prompt dispatched
by the real host.

## Storage Confidentiality

Context Mode session databases can contain user prompts, decisions, file
paths, and tool summaries. Treat both client stores as private user data:

```text
~/.codex                         mode 700
~/.codex/context-mode/**         directories 700, files 600
~/.claude                       mode 700
~/.claude/context-mode/**        directories 700, files 600
```

The private top-level directory is the durable isolation boundary for newly
created SQLite sidecars as well. On this workstation, default ACLs on each
`context-mode`, `content`, and `sessions` directory also remove group/other
access from newly created files and directories. After an install, upgrade,
restore, or manual copy, verify both modes and inherited ACLs before enabling
real prompt capture. Never index credentials, private keys, decrypted payloads,
access tokens, or configuration files that may contain them.

## What to Index

Use `ctx_index(path=..., source=...)` only for durable material that is too
large to keep in the conversation and will be searched repeatedly:

- a completed Spec directory containing Markdown contracts and decisions;
- a stable API or protocol reference;
- an external design document already stored locally;
- a maintained migration or operations guide.

Use a specific source label such as:

```text
NDNSF Spec 148 stream discovery design
NDNSF Stream API contract
```

Prefer file-backed indexing so Context Mode can detect content-hash staleness.
Re-index after an intentional document revision.

Maintain these project-layer baseline sources in each client store:

```text
Path:   .specify/memory/context-mode-project.md
Source: NDNSF project context anchor

Path:   .specify/memory/constitution.md
Source: NDNSF project constitution
```

```text
Path:   .specify/memory/context-mode.md
Source: NDNSF Context Mode operating guide

Path:   .specify/feature.json
Source: NDNSF active feature pointer

Path:   specs/<active-feature>/
Source: NDNSF active feature <exact-feature-basename>
```

The project layer may additionally index maintained documents from completed or
historical Specs with labels such as:

```text
NDNSF Spec 163-di-collaboration-planning:<absolute-path>
```

Use `scripts/context_mode_index_project.sh` for that bounded
cross-Spec refresh. The active layer remains selected by `.specify/feature.json`
and is refreshed with `scripts/context_mode_index_authority.sh`; changing the
pointer never changes the project anchor or silently changes the project DB.
Use bounded Markdown/JSON include sets; do not index source, results, build
artifacts, or logs.

Do not index:

- source trees: use CodeGraph;
- logs, test output, CSVs, packet traces, or build output: use analyzers or
  sandboxed file processing;
- `results/` directories;
- secrets, credentials, private keys, decrypted payloads, or tokens;
- temporary drafts that will immediately be replaced;
- an entire repository merely to make Context Mode appear populated.

## Empty or Stale Context

Treat Context Mode as unusable for the current task when:

- `ctx_stats` reports only fixtures, skeletons, or detection probes;
- search finds only AGENTS files or an older unrelated Spec;
- the active feature or latest user correction is absent;
- a file-backed result is marked stale;
- Codex and Claude return different histories.

Do not manufacture a summary to fill the database. Restore context from the
active Spec and source, then write any missing durable decision into the Spec,
contract, audit, or handoff file. Index that maintained document only after it
is stable.

## Hooks Configured but No Real Capture

The `ctx_doctor` MCP tool (or `context-mode doctor` from a shell) validates the
executable, storage, SQLite/FTS5, version, feature flag, and hook entries on
disk. It does not prove that Codex trusts those hook
commands, or that an already-running host loaded a configuration written after
that host started. A configured but `untrusted` hook is not executed.

This failure has a characteristic signature:

```text
ctx_doctor: PASS
ctx_stats: very few events or only skipped fixture/skeleton adapters
ctx_search: AGENTS-only results
expected project session DB: missing or contains no current user prompt
```

The repository guard asks Codex's own `hooks/list` API for every required
Context Mode hook and the NDNSF query guard. It fails with
`HOOK_TRUST_REQUIRED` if any required entry is missing, disabled, modified, or
untrusted. Approve only the exact reviewed commands and hashes. Never use a
global hook-trust bypass, and do not approve unrelated GSD or user hooks as
part of Context Mode repair. Codex binds trust to the current command hash, so
a later command change returns the entry to an untrusted or modified state.

On Codex, compare the app-server start time with the host-owned hook
configuration:

```bash
ps -eo pid,lstart,args | rg '[c]odex .*app-server'
stat -c '%y %n' ~/.codex/hooks.json
```

If `hooks.json` is newer than the running app-server, restart the full Codex
session or IDE host. Reloading MCP tools alone is insufficient because hook
registration is host-owned and loaded at process startup.

The repository health guard starts a short-lived app-server with
`mcp_servers={}` because `hooks/list` does not require any MCP server. This
keeps hook-trust validation independent of unrelated MCP startup time. It also
means that a passing guard validates Context Mode hooks, sources, and SessionDB,
but does not prove that the current IDE host has reloaded its MCP children.
After changing `~/.codex/config.toml`, reload the full IDE window/client before
testing MCP tools. Run the stable and `--scope active` health checks
sequentially; concurrent probes can race while inspecting the same SessionDB.

Do **not** use the whole-file mtime of `~/.codex/config.toml` as a restart
signal. Codex legitimately rewrites that file while the app-server is running
when it records `[hooks.state.*]` trusted hashes or unrelated UI/model settings.
The repository guard therefore validates the required semantic settings in
`config.toml`, obtains current trust/enabled state from Codex's `hooks/list`,
uses only `hooks.json` mtime for the restart check, and separately requires a
real current-project prompt in SessionDB. This prevents a normal hook-trust
write from causing a permanent false `HOST_RESTART_REQUIRED`.

Use this recovery sequence:

1. run the `ctx_doctor` MCP tool (or `context-mode doctor` from a shell), then
   run the repository `health` guard;
2. if doctor reports an installation, version, hook, MCP, storage, or FTS5
   failure, run `ctx_upgrade` and execute the exact returned command;
3. if `health` reports `HOOK_TRUST_REQUIRED`, review and approve only the exact
   Context Mode and NDNSF query-guard commands; rerun `health` and require the
   hook-trust check to pass;
4. if `hooks.json` is newer than the host, skip the unnecessary upgrade and
   restart the full client session/app-server; a newer `config.toml` alone is
   diagnostic information, not a restart requirement;
5. after restart, run:

   ```bash
   python3 scripts/context_mode_guard.py health --project-root .
   ```

   Record the new host start time, hook-trust result, and current SessionDB
   state. A pre-marker missing-event result is diagnostic, not permission to
   skip the remaining acceptance steps;
6. generate a never-before-used marker with the
   `CTX_HOST_ACCEPT_<date>_<feature-id>_<nonce>` form;
7. require both an exact search miss and the guard's absence check before
   submission:

   ```bash
   python3 scripts/context_mode_guard.py marker \
     --value CTX_HOST_ACCEPT_<date>_<feature-id>_<nonce> \
     --expect absent \
     --project-root .
   ```

8. submit the marker through a normal user prompt, never through a manual hook;
9. search the exact marker alone with `sort: "timeline"` and require a
   correct-project `user-prompt` hit whose timestamp is later than the new
   app-server start and prompt submission;
10. require the guard's presence check:

   ```bash
   python3 scripts/context_mode_guard.py marker \
     --value CTX_HOST_ACCEPT_<date>_<feature-id>_<nonce> \
     --expect present \
     --project-root .
   ```

   It must prove the exact project, `user-prompt` category, post-restart
   timestamp, and SessionDB advancement;
11. rerun `health`, then run a source-scoped relevance search for each baseline
    file-backed source;
12. reject the session and use repository fallback if any check fails.

The exact-marker search itself may later be recorded as a non-user tool event.
Such an event can never satisfy the marker guard, but it also does not
invalidate a separately verified `user-prompt` event. Before disclosure,
`--expect absent` still rejects the marker in every event category.

A manually dispatched hook fixture is useful only to separate two fault
domains:

```text
manual fixture writes + direct DB query succeeds
  -> the scripts can hash and write the cwd/session supplied by that fixture

real prompt is still absent after host restart
  -> host hook dispatch is still broken
```

Never report the first result as end-to-end success. Real prompt capture after
a restart is the required acceptance signal. Fixtures must use the reserved
`CTX_FIXTURE_*` prefix; `CTX_HOST_ACCEPT_*` is reserved for real host prompts.
`health` excludes fixture-prefixed prompt data, so a manual fixture cannot make
the real-host gate pass.

### Context Mode v1.0.169 saturated-session prompt eviction

Context Mode v1.0.169 caps each session at 1000 events. Its Codex
`UserPromptSubmit` hook originally stored the raw `user-prompt` at priority 1
and then stored the derived `intent` at priority 4. Once a long session reached
the cap, the second insertion immediately evicted the raw prompt from the same
hook invocation. This produced a characteristic failure:

```text
hook input, cwd, SessionDB, and session ID are correct
UserPromptSubmit reaches the insertion path
derived intent survives
exact CTX_HOST_ACCEPT_* user-prompt is already absent
```

This is not a Codex/Claude shared-directory collision. The clients retain
separate SessionDBs; only repository-backed documentation is shared.

Until an official release fixes the behavior, the local Codex hook keeps the
raw prompt in the same rolling-retention tier as derived intent by setting its
priority to 4 in the installed
`hooks/codex/userpromptsubmit.mjs`. Equal-priority FIFO eviction preserves the
recent prompt while still bounding the database at 1000 events. Do not treat a
manual fixture as verification. Test at the saturated cap, then require a new
real marker to pass the exact timeline search, `marker --expect present`, and
`health`.

An official Context Mode upgrade may overwrite this local hotfix. After every
upgrade, inspect the installed Codex `UserPromptSubmit` prompt event priority,
rerun the saturated-cap regression, restart the full host if hook
configuration changed, and repeat the real-marker acceptance sequence. If the
official implementation changes shape, do not blindly reapply a textual patch;
use repository fallback until its retention semantics are re-audited.

## Acceptance Matrix

| Check | Pass condition | Failure response |
|---|---|---|
| Installation | `ctx_doctor` reports hook, storage, server, and FTS5 success (shell equivalent: `context-mode doctor`) | Run the official upgrade, then re-audit the documented v1.0.169 retention hotfix before real capture |
| Saturated prompt retention | At 1000 events, the raw prompt and derived intent both survive the same `UserPromptSubmit` hook fire long enough for marker verification | Keep repository fallback active; audit prompt priority and eviction order before accepting the host |
| Hook trust | Repository `health` reports every required Context Mode and NDNSF query-guard hook enabled and `trusted` or managed | Review and approve only the exact command hashes; never enable a global bypass |
| Host dispatch | A previously absent, post-restart `CTX_HOST_ACCEPT_*` marker submitted only through a real prompt returns as `user-prompt` after the new host start | Restart full host; if still absent, repository fallback |
| Project binding | Result is attributed to the current repository and its exact SessionDB advances | Reject wrong-project/session data and inspect project root |
| Shared guide | Source-scoped relevance query returns this file and is not stale | Re-index this exact repository path |
| Active feature | Exact feature basename and identifiers return current Spec material | Read pointer/Spec directly, then re-index stable files |
| Query quality | Results include requested identifiers, not only AGENTS text | Replace generic terms with high-entropy identifiers |
| Authority | Retrieved claims agree with current files and source | Files win; mark Context Mode stale |

## Codex and Claude Code

The clients may keep separate private session-event stores:

```text
Codex:       ~/.codex/context-mode/
Claude Code: ~/.claude/context-mode/
```

Private session separation is acceptable; documentation divergence is not.
Both clients MUST read or file-index these exact repository paths:

```text
.specify/memory/context-mode-project.md
.specify/memory/context-mode.md
```

Both use the same source labels:

```text
NDNSF project context anchor
NDNSF Context Mode operating guide
```

When a maintained repository document is useful to both clients, each client
indexes the same file path into its own store. Never copy the document under
`~/.codex/` or `~/.claude/`, because those copies will drift and file-backed
staleness checks will no longer refer to the repository authority.

Successful capture in one client still does not prove that the other can
retrieve the same private conversation. Cross-client task continuity MUST use
the same repository files:

```text
.specify/feature.json
spec.md / plan.md / tasks.md
contracts/
traceability.md
audits and closure reports
handoff files for interrupted work
```

Do not configure a shared Context Mode storage directory casually. Concurrent
clients, session identities, permissions, backup, and cleanup must be validated
before adopting one shared path. Repository artifacts remain authoritative
even if shared storage is later enabled.

After changing either project-layer guide, re-index the same repository files
in both clients. A project-lane search scoped to the anchor or guide must return
the revised repository-backed section in each client.

Run each client's guard against its own store:

```bash
python3 scripts/context_mode_guard.py health --platform codex --project-root .
python3 scripts/context_mode_guard.py health \
  --platform claude-code \
  --project-root .
```

## Keeping the Active Feature Synchronized

The active Spec pointer is mutable. Changing `.specify/feature.json` does not
automatically replace the file-backed entries in Context Mode's ContentDB.
After creating or switching a feature, run the repository repair/index helper
before relying on Context Mode:

```bash
scripts/context_mode_index_authority.sh
```

The helper indexes exactly the current pointer, the project anchor, this guide,
and the active feature's `spec.md`, `plan.md`, and `tasks.md`, then runs the
project-layer fail-closed guard. It does not replace the optional full
cross-Spec refresh:

```bash
scripts/context_mode_index_project.sh
```

That refresh indexes only maintained Markdown/JSON files under `specs/` and
the stable project guides, then invokes the active helper. It never indexes
source, results, logs, or build output.
It is safe to rerun and does not index source trees, results, logs, or build
output. This repair is required because changing `.specify/feature.json` does
not migrate or invalidate the old file-backed sources automatically. A missing
project ContentDB entry is therefore an index-sync failure, not evidence that
the other client has stolen or corrupted this project's context.

For Claude Code's separate store, run the same helper from Claude's process
environment (or explicitly set `CONTEXT_MODE_PLATFORM=claude-code` and its
`CONTEXT_MODE_DIR`); `claude-code` is the canonical platform identifier. Do not
merge the Codex and Claude ContentDB directories,
and do not delete the other platform's store to repair a stale active-feature
index.

The repository helper accepts that platform selection directly. For example,
to refresh Claude Code's private ContentDB without touching Codex's store:

```bash
CONTEXT_MODE_PLATFORM=claude-code \
CONTEXT_MODE_DIR="$HOME/.claude/context-mode" \
scripts/context_mode_index_authority.sh
```

With no platform override, the helper targets Codex's private store and runs
the Codex guard. The two commands must be run separately; never point both
clients at one ContentDB.

Claude's global `~/.claude.json` must register both MCP servers independently
of `settings.json`. Use absolute executable commands; the CodeGraph server
must include `serve --mcp --no-watch`, and the Context Mode server must set
`CONTEXT_MODE_PLATFORM=claude-code`. A stale per-server value of `claude` can
make Claude write to the wrong store even when the settings hooks look valid.
The repository guard checks this registry, the executable paths, and the
canonical platform value before accepting Claude host health.

After creating or changing the active plan, keep `.specify/feature.json` and
the managed `<!-- SPECKIT START -->` block in `AGENTS.md` on the same feature.
The `agent-context` extension is optional. A declaration in
`.specify/extensions.yml` does not prove that its scripts were installed.

If
`.specify/extensions/agent-context/scripts/bash/update-agent-context.sh`
exists and is executable, it may update the managed block. If it is absent,
edit the feature pointer and managed block together; never report a successful
automatic update. In either case, require:

```bash
active_dir="$(jq -r '.feature_directory' .specify/feature.json)"
test -f "$active_dir/spec.md"
test -f "$active_dir/plan.md"
test -f "$active_dir/tasks.md"
rg -F "at $active_dir/plan.md" AGENTS.md
```

Verify:

```text
.specify/feature.json points to the same feature
AGENTS.md managed block points to that feature's plan.md
tasks.md reflects the real checkpoint
unfinished work has no false completed checkbox
```

For long or benchmark-heavy work, also write a GSD resume or handoff artifact
containing:

- objective and explicit exclusions;
- last verified task;
- current processes/output paths;
- exact next command;
- frozen result paths that must not be modified or rerun;
- known failure and retry state.

## Installation and Upgrade Verification

Use Context Mode's own doctor and upgrade operations. Run the `ctx_upgrade` MCP
tool (or `context-mode upgrade` from a shell) only when `ctx_doctor` identifies
an installation, version, hook, MCP, storage, or FTS5 failure, and execute the
exact command it returns.

After any installation, upgrade, or hook/configuration change:

1. restart the full Codex session/app-server and reload plugins or restart
   Claude Code;
2. run `ctx_doctor` again inside each restarted client (or
   `context-mode doctor` from a shell);
3. follow the complete recovery and acceptance sequence in
   [Hooks Configured but No Real Capture](#hooks-configured-but-no-real-capture);
   do not substitute a shortened marker check;
4. search source `NDNSF Context Mode operating guide` in relevance mode and
   confirm that both clients return this repository-backed file, not a private
   copied document.

An installed server with an empty database is not a successful context
continuity test. The acceptance condition is retrieval of a real, current
prompt or decision from a restarted client session.

### Codex log-database recovery

If `codex doctor` reports that `~/.codex/logs_2.sqlite` has failed integrity
checks, do not run `VACUUM`, `REINDEX`, or replace the file while the Codex
app-server still has it open. First close the full IDE/Codex client and verify
that no `codex app-server` process owns `logs_2.sqlite`, its `-wal`, or its
`-shm` file. Then move those three files together to a timestamped backup
directory (never delete them), restart the client so Codex can rebuild the log
database, and rerun `codex doctor`. Finally rerun both repository health
scopes and submit one real post-restart prompt before accepting host capture.
Stale rollout rows are diagnostic warnings; do not bulk-delete rollout files
as part of this repair unless a separate retention decision has been made.

The repository includes a guarded helper for this operation:

```bash
scripts/codex_state_recovery.sh --check
# after closing the full VS Code/Codex client:
scripts/codex_state_recovery.sh --apply
```

The helper refuses to move any file while a process owns the database trio and
moves existing files to a timestamped, mode-700 recovery directory. It never
deletes rollout files or the backup. `--apply` must be followed by a full
client restart; a new shell alone is insufficient.

### Codex MCP and plugin checks

Treat the local MCP registry and the plugin marketplace as separate layers.
`codex mcp list` must show the configured `context-mode` and `codegraph`
servers as enabled, and the `ctx_doctor` MCP tool (or `context-mode doctor`
from a shell) must report their executable, storage, FTS5, and hook checks as
passing. `codex plugin list` reports only plugins from
configured marketplace snapshots; `No marketplace plugins found` therefore
means that no CLI marketplace is configured, not that an enabled MCP server or
repository skill is broken. Do not repair this state by editing the plugin
cache or deleting app data. If a marketplace or app-backed plugin is required,
add or refresh it through the Codex plugin UI/CLI, then restart the full Codex
client and rerun `codex mcp list`, `codex plugin list`, and `ctx_doctor` (or
`context-mode doctor` from a shell).

On the current Codex build, `codex features list` reports the former
`plugin_hooks` flag as removed. Do not keep `plugin_hooks = true` in
`~/.codex/config.toml`; it is not needed for the explicit `~/.codex/hooks.json`
fallback and can make the installation appear to depend on an obsolete
feature. The required runtime flag is `[features].hooks = true`, together with
the reviewed Context Mode and NDNSF guard commands in `hooks.json`.

Installed local skills are checked from their repository paths, independently
of the remote plugin catalog. A skill being present on disk does not prove that
the already-running IDE host has loaded a changed skill or hook; a full client
restart remains the acceptance step after installation or configuration
changes.

## Reporting

For substantial work, the final report should state one of:

```text
Context Mode: used; current feature/checkpoint results matched repository state.
Context Mode: empty or stale; repository fallback used.
Context Mode: unavailable after bounded diagnosis; repository fallback used.
```

Always report source verification, Spec Kit/GSD/ARS usage when applicable, and
what remains. Never present Context Mode retrieval as measured runtime or
experiment evidence.
