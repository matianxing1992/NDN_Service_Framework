# DSH Validation Queue

Local asynchronous validation for DSH 0.2.2-rc.1. One plugin-owned verification slot runs
source review → approved commands → result analysis. The author keeps coding in a separate,
fixed worktree. Results identify the candidate, artifacts and whether the author's tree advanced.
No second model loop owns the queue, and no additional NDNSF build tree is created.

## Ownership and versions

- **Author**: one fixed coding worktree. `validation_submit(profile, title, dependsOn?)` captures
  the whole current source tree, including staged, unstaged, new and deleted inputs. It uses a
  private Git index and `refs/dsh-validation/UUID`; it never stages or commits the user's branch.
- **Validator**: a distinct worktree of the same repository. It must be clean, have no resident
  DSH agent and be explicitly selected in the GUI. It switches to detached private candidates;
  it exclusively reuses the two existing build trees. Branch refs are not moved. It does not
  restore the prior checkout automatically on disable; the final detached candidate remains
  identifiable. No checkout/reset is performed merely by installing, saving disabled settings,
  or inspecting status. Existing unrelated changes are never discarded.
- **Commands**: a human-approved GUI profile supplies exact executable/argument/environment
  lists, acceptance text and actual artifact paths. Model tools can choose only a named profile.
  There is no model-supplied shell command and no automatic installation or publication.
- **AI**: each review/analysis uses a short-lived independent root agent, linked by session
  metadata but **without `parentAgent`**. Native parent goal rounds therefore do not join it.
  Only frozen-file/diff/results reads and a structured verdict tool are available. It cannot
  edit, run shell, create goals, delegate, or override a failed process exit code. Its provider,
  model and effort inherit the submitting agent; no fallback model is selected by this plugin.
  The compiler is a plugin-owned process group, not an LLM waiting on a tool. Only one stage
  runs at a time. Native source/permission/model errors remain errors, never a synthetic PASS.

## Setup

Install the bundled tarball through `dsh plugin --profile web add /absolute/package.tgz`.
The conversation composer gets a **验证队列** button; the panel is closed by default.
Installation starts no model request, build, workspace migration or goal.
The host-interface self-check creates and immediately drains an independent worker without
requesting a model response. A separate explicit model-transport probe is available through
the authenticated control interface (`probe`, `model: true`); it makes a bounded fixture-only
model request and does not constitute code/build/inference validation.

On first use, click **设置并开启** or **开始设置**. The button remains usable before a
configuration exists. Read-only discovery lists actual worktrees, their branch/HEAD, pending
changes, existing build configuration and resident DSH sessions. Select the author and
validation directories in the labelled fields; expand the advanced section to approve the
named command profiles and their actual artifact paths. Click **保存并开启**. A failed check
leaves dispatch disabled and explains the specific reason in Chinese; failed saves retain the
edited form. Detection never creates a worktree, moves a session or changes source.

The same settings remain under **工作区与已授权命令配置** (`authorRoot`, `validationRoot`, `profiles`).
The two roots must exist and be Git worktrees of the same repository. Use absolute real paths.
Keep ordinary NDNSF compilation in the original `build/` and ASAN/UBSAN in its existing
`build-ASAN-UBSAN/`; the author worktree has no build tree. Do not copy/link mutable build
directories between worktrees. Run the environment self-check, then enable from the coding
session. A loaded DSH session in the validation checkout prevents takeover, even if idle.
End or migrate the old workflow deliberately; the plugin never forks/moves an active goal.
Model worktree switches must use DSH's supported new-session/fork flow; session cwd is immutable.
Ignored Specs, model data and credentials are not silently copied into Git. Make required
task inputs available through their declared owners before moving an existing task.

Example (verify actual Waf output paths/configuration before enabling):

```json
{
  "enabled": false,
  "authorRoot": "/path/to/coding-worktree",
  "validationRoot": "/path/to/original-checkout",
  "profiles": {
    "unit": {
      "acceptance": "Actually execute selected asset-free cases; missing resources, skip and zero-match cannot be PASS.",
      "artifacts": ["build/unit-tests"],
      "commands": [
        {"kind":"build","file":"./waf","args":["build","--targets=unit-tests","-j6"]},
        {"kind":"test","file":"python3","args":["tests/run-checks.py","--profile","unit"],"timeoutMs":3600000}
      ]
    }
  }
}
```

Profiles can also invoke maintained installed/MiniNDN launchers. Their permission, model,
credential and hardware prerequisites remain real prerequisites. No additional root privilege
or sanitizer/global-install exception is granted by this plugin. Specify the actual selected
ELF/shared-library/installed paths in `artifacts`, not just a logical target or symlink name.
Artifacts (up to 32, each at most 512 MiB) are SHA-256 checked before tests and after execution.
An install/profile should include its real manifest and dependency identities among inputs/checks.
Zero exits plus AI acceptance cover only the configured profile, not an entire Spec or goal.

## Queue and recovery

- Capacity is **one running + one queued**. Submit at a coherent batch boundary; do not submit
  every file save. An additional submission returns backpressure, without capturing another tree.
- `dependsOn` blocks execution unless that exact preceding job passed. Only independent work may
  proceed before verification. The plugin does not rewrite the Spec's dependencies or completion rules.
- A changed author tree is `historical-only`; this first version deliberately does not infer that
  an old PASS applies to a later tree. Identical-tree results still have a profile/environment scope.
- Each repository has an atomic build lease shared across plugin instances/worktrees. Stale
  locks are reported, never automatically stolen; inspect recorded PID/job/process effects first.
  This lease does not control external shells/Codex: external writers/builds must also respect it.
  Source/HEAD/artifact drift detected after execution invalidates the result. The two-scan capture
  rejects observed drift, but is not an OS filesystem snapshot against uncooperative outside writers.
- Disabling pauses admission and subsequent dispatch; it lets the current command finish.
  Cancelling aborts the process group and drains the worker before another candidate is loaded.
- Restart marks unknown in-flight work `interrupted`, disables dispatch, and retains pending work.
  It never blindly replays an installation or other possibly non-idempotent command.
- Result messages have stable IDs. Active/running authors receive steering at a step boundary;
  paused/blocked/complete goals are not awakened. Failed deliveries retry up to five times while
  a live active author is available, or via the history button. No additional native goal is created.
- At most 22 compact job records/private candidate refs are retained (20 history + 2 in flight).
  Output is capped at 2 MiB per command, retained tails at 36 KiB/command; profiles have ≤8 commands.
  Source capture is limited to 20,000 files/256 MiB, excludes secret-like **untracked** paths,
  and refuses absolute/external source symlinks and submodule directories without an explicit closure design.
  Git object pruning follows normal repository maintenance; ref retention is not a byte quota.
  No model/build/install archives are created. Review sessions are disposed after each bounded phase.

## Existing children

The authenticated, explicitly confirmed handoff operation uses native `drainChildren` for an
enumerated set of the parent's direct children. It saves their task/report cutoffs, preserves
source and durable sessions, and sends only caller-specified unfinished work to the parent.
It does not use `drainDescendants`, which would close future admission for that live parent.
The most recent handoff is visible in this parent's queue panel. Historical child catalog rows
remain historical; released children are not deleted or automatically resumed. Future task
delegation remains the main agent's decision. Queue workers run only after an explicit submit.

## Reuse audit

Reviewed upstream `zibo2025/dsh-orchestrator@76c0368b45fa6a8be25f581cc9af3cbe2ebf0e48`:
`src/index.ts` calls native `ctx.subagents.startContinuable/followup`; package peers target the
older 0.1.0-rc.6 contract. It supplies messaging/mesh, not frozen source/build leasing or a
versioned verification queue. Installing it would not remove native goal-driver child joins.
This plugin reuses the installed `agents.create`, agent preset composition, scoped tool
restrictions, session events/inbox, Typert, storage domains and Web composer slot instead.
No upstream source was copied and no DSH core, third-party library or existing audit plugin changed.

## Verification

`npm test` after installing dependencies. For testing against an existing host installation
without copying its dependencies, use Node 24 and set `DSH_TEST_ANCHOR` to that DSH package.json,
then add `--import ./test/host-modules.js` to the test invocation. Tests use disposable small Git
repositories and commands; they do not compile NDNSF or demonstrate MiniNDN inference.
See `verification.md` for the actual installation, runtime and remaining qualification boundaries.
