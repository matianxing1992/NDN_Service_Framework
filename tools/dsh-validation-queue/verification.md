# Validation queue delivery — 2026-11-16 20:44 -06:00

## Delivered scope

`dsh-validation-queue@0.1.4` is installed and loaded in the existing DSH Web profile.
No DSH restart, existing goal creation/pause/resume, NDNSF build/install/inference or workspace
migration was performed. The queue has no project configuration, is disabled and contains zero
jobs. Existing model caches, credentials, builds and unrelated worktree/index edits are preserved.
Source scope: this new `tools/dsh-validation-queue/` only; no commit/push was requested or made.
The surrounding DSH development progressed independently to HEAD `6df61a620736` during this task.

## Upstream/source review

Pinned `zibo2025/dsh-orchestrator@76c0368b45fa6a8be25f581cc9af3cbe2ebf0e48`.
Read `src/index.ts`, its native `startContinuable/followup` integration and package peers.
It targets older 0.1.0-rc.6 APIs and does not own source snapshots/build scheduling. Reused the
installed native APIs rather than installing another mesh. No upstream code was copied.

Actual paths reviewed: native agent registry create/setup/disposal, preset composeFrom,
tools defineTool/restrict/output schema, goal-round-driver child joins, subagent drainChildren
versus drainDescendants, session event/source/inbox attribution, Typert Remote signatures,
storageDomain tables and pluginManager/setBundleEnabled. Used the installed read-only
review-agent protocol for the affected new source; no independent reviewer agent was spawned.

Self-review corrections include explicit native output schemas, non-destructured Typert method
parameters, absolute author-source symlink rejection, actual artifact resolution/hash checks,
stopping dispatch on storage failure, user cancellation ownership and approved-profile own-key
validation. Existing behavior gates were not relaxed to get a passing result.

## Tests actually executed

Node: `/home/tianxing/.local/share/deepseek-harness/runtime/node-v24.22.2-linux-x64/bin/node`.
Final command (24 tests, exit 0):

```sh
DSH_TEST_ANCHOR=/home/tianxing/.local/share/deepseek-harness/app/lib/node_modules/@deepseek-ai/dsh/package.json \
/home/tianxing/.local/share/deepseek-harness/runtime/node-v24.22.2-linux-x64/bin/node \
  --import ./tools/dsh-validation-queue/test/host-modules.js \
  --test --test-reporter=dot tools/dsh-validation-queue/test/*.test.js
```

20 tests exercise real disposable small Git repositories and owned processes: private-index
staged/unstaged/new/deleted capture, user index/branch preservation, candidate immutability,
dirty-root refusal, cross-worktree lease, source-read containment, absolute/external links,
FIFO/backpressure and no overlapping commands, exact/historical result scope, failure retained
despite AI accept, failed dependencies, configuration freeze, interrupted restart, source/artifact
drift, profile validation, timeout/output bounds, active cancellation/lease recovery and storage
failure stopping dispatch. Four additional tests use the real installed defineTool compiler with
mock agent lifecycle and selective-handoff ownership fixtures. These are not full DSH lifecycle
or NDNSF qualification tests.

Restricted execution initially swallowed nested Node output even for `node -p 40+2`; a matching
unsandboxed constant-only probe printed 42. Final process tests ran with execution approval in
the real host boundary. No test assertion was removed or weakened for this environment issue.

## Actual live DSH checks

- Loaded through official `pluginManager/setBundleEnabled`, application=applied.
- Authenticated `validationQueue/view` succeeded, config=null and running/queued=0.
- A no-model host probe created a root worker with no author ownership, then disposed it.
  Observed `root=true`, `ownedByAuthor=false`, `modelRequestExecuted=false`.
- Explicit model/tool transport probe session:
  `validation-62ccd39a-30c5-433d-9236-942f4b86554a`.
  The independent read-only worker consumed the supplied arithmetic fixture and used
  `validation_read`/`validation_verdict`, returning accept before disposal. Its durable
  assistant events 14/19/24 show provider `open-design`, model `deepseek-v4.1-flash`.
  This was one probe turn with three model responses/tool rounds, not a single provider request.
  Requested effort was unspecified; no actual effort confirmation is claimed.
  The command record in this transport probe was a synthetic fixture, not an executed build
  or inference. Its accept result proves only native model/tool/report transport, never product
  behavior or mathematical model accuracy.
- Final main goal: `goal-ba31e68e-79bd-4fd9-acec-78f93942ec66`, revision=5,
  phase=active, activation=armed. Native rounds progressed from 5 to 6; this plugin did not
  create or edit that goal. The host was not restarted.

## User-authorized existing-child handoff

User explicitly confirmed ending existing children and transferring only unfinished work.
Used native `drainChildren` for precisely:

- `b542ff19-440d-42b1-9f1e-2cfc6e80983f`: KV investigation, final cutoff 3156.
- `8724805f-e85f-4a4c-b5c3-1feae3963a78`: nine integration failure investigation, final cutoff 1528.

Both were confirmed unloaded after drain. Source changes, logs and durable sessions remain.
Record `e03fe408-b23b-44d2-a9f6-fa5d4e6f8c6e` reached delivered state and survived plugin reload.
Message `validation-handoff-e03fe408-b23b-44d2-a9f6-fa5d4e6f8c6e` entered the original parent's
durable conversation at seq 77723. It assigns only remaining production-rank replay and the
unfinished integration rows/report, with existing evidence references and original constraints.
It does not request redoing completed work or resurrecting the old children. No permanent
parent admission cutoff was used; future delegation remains the main agent's decision.

## Exact artifact

Persistent package:
`/home/tianxing/.dsh/profiles/web/plugin-packages/dsh-validation-queue-0.1.4.tgz`

SHA256: `d8a1c96139c6807f6fcc4aacd20adca46b8e6ed8193c6356c75703afe1be1ec5`.
All ten packaged files were byte-compared against maintained source and installed files;
all matched. These verification notes and tests stay with the maintained source, not in the
runtime package. pnpm reported host-provided native peer declarations as missing at profile
level; real host import, service RPC and model/tool lifecycle checks succeeded. Existing React
peer warnings on other plugins were unchanged; no global dependency upgrade was performed.

## Remaining limits

No browser visual inspection, NDNSF native build, MiniNDN run, global install or end-to-end
real-project queue qualification was performed. Syntax checks do not prove browser rendering.
Before enabling, deliberately select two real worktrees and approved profiles/artifacts; the
validation root must be clean and have no resident author agent. Current active NDNSF work
was not migrated or displaced. The plugin does not enforce OS-level isolation against external
writers, infer old-result applicability to changed trees, solve missing project data, or replace
independent native acceptance oracles. Stale build leases require inspection, not automatic theft.
No claims of measured speedup or reduced total token usage are made.

Task closeout removes only this task's upstream inspection clone, release cache/staging and
unreferenced intermediate tarballs. The current package, source/tests and handoff history remain.

## First-use activation repair — 0.1.5

The user reported that the plugin could not be enabled. Live inspection confirmed that
configuration was null, with zero queued/running jobs. The old button was disabled when no
configuration existed, without a first-run setup route. This was a UI defect, distinct from
the real clean/idle validation-workspace prerequisites.

Changed only this plugin: added read-only worktree discovery, labelled directory fields,
an enabled first-run settings button, save-and-enable with genuine preflight, and Chinese
reasons for blocked activation. An invalid save retains the edited form. Discovery reports
actual HEAD/branch, dirty state, maintained build configuration and resident DSH agents;
it never creates worktrees, changes HEAD, clears pending work or moves sessions. Git on this
machine does not support `worktree list -z`; discovery uses compatible porcelain output,
including ordinary paths containing spaces.

The same full test command above passed **27 tests, exit 0** after this repair: 24 existing
tests and three new first-use tests for read-only discovery, missing/dirty/clean activation
and distinguishable Chinese errors. The actual installed native host module import and client
syntax check also passed. No NDNSF product build or inference was launched.

Installed via the maintained CLI and hot-loaded with official `setBundleEnabled`; no DSH
restart or goal mutation was requested. Authenticated live checks confirmed:

- `setup` returns real worktrees and `needs-setup / CONFIGURATION_REQUIRED`.
- Enabling without configuration returns an explicit Chinese reason and retains config=null.
- Counts remain queued=0 and running=0; the existing child handoff record is retained.

A separate temporary Chrome profile opened the existing conversation. Actual browser checks
passed: the first-use switch was clickable, clicking it expanded settings, both directory
fields rendered, detection listed actual worktrees, and saving an incomplete form showed the
specific path-selection error. The panel screenshot was visually inspected. No model message,
profile save, build or workspace migration was performed by this UI check.

Package: `/home/tianxing/.dsh/profiles/web/plugin-packages/dsh-validation-queue-0.1.5.tgz`.
SHA256: `81f075ecfb2c5224dd01fd3a5488dd6cef9e471701060dcdcbbd0a07b0e1d323`.
All 11 packaged files match maintained source and installed files byte for byte.

Real-project dispatch remains unconfigured, rather than falsely enabled: the original
checkout has pending edits and resident DSH sessions, and owns the two maintained build
trees. It cannot safely become the validator while also being the active coding directory.
An explicitly arranged separate author worktree and a clean, idle validation checkout are
still needed before real queue use. Existing worktrees were not repurposed. The browser
smoke and fixture tests do not constitute a real-project end-to-end queue qualification.
