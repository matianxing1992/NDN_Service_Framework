# DSH Codex parallel audit

Development version. The host independently schedules read-only Codex audits while
DSH continues its task. The conversation composer provides an audit-history panel.
Select **审计模型** and **思考强度** in the automatic audit card. The host reads
listed models and their supported effort levels from the local Codex model
cache at plugin startup. Without a readable catalog the panel offers the
existing `gpt-6.2-sol` settings. Changing models keeps the previous effort if
supported, otherwise it uses the new model's default. Both selections are
saved per session for future manual, scheduled, and blocked-goal audits.
Changing either preserves the next scheduled audit time.

Click **Codex 审计** beside the conversation composer to open the side panel and
use its automatic-audit switch. **立即审计** first opens a dialog with the configured
default prompt; clear it to type a prompt for that one audit. A per-run prompt is
never saved and does not change the automatic audit's configured prompt. The panel starts collapsed, closes on Session
switch or page hiding, and never opens merely because an audit completed.
Reports and responses use DSH's Markdown renderer; connection settings start
folded. Response controls locate the original message using DSH's history loader
and current Chat row identities, and report unavailable targets explicitly.
The default interval is three
hours, adjustable to integer values from one to five hours. Scheduling runs in
the DSH Host, including while the panel is closed. A stopped Host cannot run audits.
Sessions start disabled so installing the plugin does not unexpectedly spend quota.
With automatic audits paused, **预约下次审计** saves a single run due one
configured interval from the click. The displayed time stays fixed if you run
an immediate audit or change the interval. **取消预约** removes the pending run.
At maturity the Host runs it when the single audit slot is available, even if
the session is idle or the panel is closed. An overdue reservation survives a
Host restart; a started reservation is recorded once and clears its timer.
Each off-to-on transition captures a new session cursor and workspace file-hash
baseline. First periodic review covers work since that activation; subsequent
successful full reviews advance coverage to their **snapshot cursor**, not their
completion time. Turning off/on resets the period; interval edits, skipping and
failed/cancelled reviews do not. Existing enabled pre-upgrade settings establish
a baseline at their first post-upgrade audit, without inventing an old enable time.
The exact legacy default prompt migrates to the goal-centred window prompt;
custom prompts and historical dispatched requests are preserved.

Manual review covers outstanding work in the enabled period. When disabled or
when no new messages/files exist, it explicitly looks back one configured
interval; no historical file snapshot means code changes in that lookback cannot
be determined. Blocked reviews focus on the current blocker and never consume
full-work coverage. Reports completing after off/on cannot advance a new period.
History shows the focus window, message/file counts and coverage limits. Closed
periods are not automatically backfilled. Full conversation parts remain available
as background, while `focus.txt`, `scope.json` and `source-changes.json` identify
the primary input. Missing snapshot evidence is never proof that work did not occur.
Hash comparison identifies changed files, not changed lines; the previous file
contents are not retained. Baselines obey the same bounded source/secret filters
as audits and create no extra source-tree copy. No baseline hashes are returned
by the GUI polling endpoint.
The **跳过下一次** button advances the next deadline by one configured interval
without disabling periodic audits or cancelling the current audit. The skipped
deadline and replacement deadline are durable and shown in the panel. Disabling
the timer and cancelling an audit are separate controls.
Only one audit runs on a host at a time. Missed intervals are coalesced.
Starting a new manual, scheduled or blocked-goal audit replaces the current
periodic deadline with its start time plus the configured interval. Thus
**立即审计** consumes the old scheduled slot instead of leaving a second audit
due shortly afterward. Joining an already running audit does not reset the clock
again. A manual audit does not enable a disabled periodic schedule. History rows
identify manual, scheduled and blocked-goal triggers.

## Blocked-goal audit and continuation guard

The independent **阻塞审计与续跑守护** switch binds authorization to the current native
goal ID. It starts disabled for every session. The panel shows the native goal's
phase, activation, round budget, blocker and recovery history (latest 30 entries).
Enabling or disabling the guard does not reset the audit deadline. Disabling it
does not pause an already armed native goal; use DSH's native goal controls to pause.

The existing host maintenance tick checks loaded, opted-in sessions. An active,
disarmed goal can automatically resume only with a recorded `disposed` turn
boundary after its latest goal change, without a subsequent human decision or
execution error. Human inputs and manual/control cancellation veto automatic
restart recovery until the native goal is explicitly resumed. Paused, complete,
changed or exhausted goals are never automatically resumed. Pending inputs,
running work and a missing model also prevent recovery. Unknown interruption
causes remain visible for manual review. Historical unloaded sessions are not
opened automatically.

Recovery uses the native `goals.resume(agent, { id, revision })` contract. It
does not inject “continue” messages, create another goal loop, raise the round
budget, or replace the existing fault-recovery plugin. State is rechecked after
durable admission and immediately before native mutation. Failed recovery has
20/40-second backoff and at most three attempts; uncertain crash outcomes require
manual review. **恢复当前目标** explicitly authorizes recovery of an active goal
after checking its displayed revision, including a previously stopped active
goal. It cannot resume a paused or blocked goal.

An armed goal idle for 60 seconds is marked potentially stalled; the guard does
not enqueue competing model requests. The native driver remains authoritative.
An enabled audit schedule may also audit an idle unfinished goal bound to an
enabled guard, so stopping execution no longer silently disables its audits.
Periodic audits still use their configured 1–5 hour interval; goal checks reuse
the host's 10-second maintenance tick. Status checks make no model requests.

A native `goal/changed` block event immediately requests an independent Codex
audit; already blocked opted-in targets are also discovered by maintenance. This
does not wait for the periodic deadline, even when periodic auditing is disabled.
An occupied audit slot leaves the blocked request queued. One goal revision owns
one audit; three consecutive blocks with the same reason stop automatic cycles
and expose an explicit retry control. Source changes during capture retry with
bounded backoff (at most three captures).

The structured `blockedRecovery` decision distinguishes an executable repair plan,
missing user inputs and waiting for external conditions. The durable report and
ordered steps are delivered through the existing deduplicated DSH inbox. Only an
`actionable` plan, a fresh completion source check, actual report admission into
DSH history, unchanged blocked goal ID/revision and remaining goal budget permit
native `goals.resume`. This authorizes DSH to work on the plan; it does not claim
the blocker is already fixed. Waiting decisions retain blocked status and ask DSH
to explain what is needed. Pausing/changing the goal or disabling the guard during
the audit holds its report without waking the session. Recovery errors back off
and stop after three attempts. The GUI shows the plan, progress and recovery events.
The auditor is authorized to choose solutions within the existing objective and
project conventions: public downloads, compatible database choices/configuration,
dependencies and resource alternatives are executable plans, not reasons to ask
the user. It should prefer existing project paths and provide acquisition and
validation steps. Only genuinely unavailable private credentials, payment
authorization, indispensable user-only information or external conditions with
no feasible alternative require waiting. The auditor decides the plan; DSH
performs downloads, installation, implementation and verification under existing
permissions and explicit resource bounds.

Install updates without global HMR reconciliation during active DSH work.
Verify a safe idle window or leave a running host on its current code until it
can be restarted; installed package bytes alone do not prove live activation.

Each audit captures Git HEAD, working source files (including uncommitted and
untracked source), the ignored active Spec selected by `.specify/feature.json`,
and an exact DSH conversation cutoff through `sessionQuery`. Complete settled
messages and tool calls are retained in ordered, hashed chunks; duplicate stream
frames are excluded. A source manifest and conversation index locate the inputs.
Bounded scratch snapshots use the DSH host's OS temporary directory (`TMPDIR`
on Linux/macOS, `TEMP` or `TMP` on Windows) and are removed after the attempt.
Set the temporary directory outside the audited worktree in the environment
that starts DSH. The snapshot starts at the session's Git worktree root,
including when the session opens a subdirectory. Credentials, model-sized
assets, build outputs and symlinks are
excluded. A changing source fails snapshot admission rather than silently mixing
versions. This is a source snapshot, not a release or runtime qualification.
HEAD is only the baseline: audit instructions, history and delivered messages
identify the captured working tree, its timestamp and staged/unstaged/untracked
path changes (including deletions). Current files take precedence over commits
mentioned in historical conversation. Every audit captures afresh; it does not
reuse a previously audited commit. DSH can continue changing files during an
audit; the completion recheck marks subsequent changes for review rather than
claiming a running audit continuously observes future edits. Source/security
exclusions above still apply.

Codex uses an independent process, JSONL completion events, a structured response,
read-only shell sandbox and no approvals. User configuration is excluded to avoid
inheriting writable MCP tools and hooks; authentication still uses `CODEX_HOME`.
The plugin parses the host's TOML and reads the selected provider, including a
provider selected by the default profile. For a custom provider it passes only
supported connection fields to the isolated invocation. Model and effort are confirmed
from the exact Codex thread's `turn_context` runtime records, bounded by this
audit's completion time. Existing history is backfilled on plugin initialization.
The UI identifies this evidence as Codex runtime confirmation; it does not claim
knowledge of a server's internal routing. Missing logs retain an explicit reason
instead of inventing a confirmation; contradictory runtime selection is an error.
The plugin does not control
the VSCode sidebar or promise its audit thread appears there.

Completed reports are saved before delivery to the original DSH session via
`steer()`. This does not cancel the current model request or tool. DSH reads the
report at its next model step. The conversation shows the latest completed audit
in its input area, even while the agent is busy. The card refreshes about every
two seconds in a visible tab and loads the report body when expanded. Its receipt
states distinguish a saved report, admission to DSH history, and an explicit
DSH acknowledgement. Closing the card persists its audit ID for that session.
The next completed audit can appear, and older reports remain in audit history.
Native audit input rows stay hidden even when the card is closed. Stable message
identities deduplicate inbox and
durable-history delivery. Inbox acceptance is displayed separately from confirmed
log admission. A dropped pre-step input returns to pending delivery; automatic
delivery has at most five attempts before explicit retry is needed. A Session
without provider/model cannot read a report and receives an actionable error.
Explicit `审计回应 AUD-…` assistant messages after the admitted report are linked
to their corresponding report; ordinary subsequent messages are not acknowledgements.

Document corrections are returned as proposals with original file hashes and
before/after content. Once the report is delivered, factual corrections can apply
automatically when DSH is idle and the source recheck has not found changes.
Stale or unverified reports retain proposals for explicit review. GUI application
also requires an idle DSH session and matching current content. Conflicts preserve
the current document. Spec goals, plans,
task lists, contracts and agent instruction files remain proposals requiring review. Product source is
never an allowed document target. Concurrent external editors remain independent
writers; check the displayed diff before applying a document correction.

The history panel displays the prompt, snapshot identity, requested and confirmed
model settings, timestamps, duration, report, document proposals, delivery status,
actual DSH responses and actionable errors. RPC uses DSH's authenticated `/api`
gateway with session resource metadata; the plugin exposes no separate HTTP route.
History lists are paginated and fetch report bodies on expansion; each result is
bounded to 1 MiB. History retains up to 200 records while preserving pending deliveries. Interrupted
audits are marked on restart. Auth/model/executable configuration failures pause
automatic audit requests. Transient Codex failures have at most three attempts.

## Install on another machine

On the machine running the DSH Web host, check `dsh --version` before installing.
This package declares DSH `0.2.2-rc.1` with its Cordis `4.1.0` runtime contracts.
That DSH release requires Node.js 24.9 or newer. Install the tarball from an
absolute path on that machine:

```sh
dsh plugin --profile web add "/absolute/path/to/dsh-codex-audit-0.1.25.tgz"
```

Use the profile that actually serves your Web UI if it has another name.
The package manager needs access to the declared npm dependencies or a
prepopulated cache. Verify `dsh-codex-audit` occurs once in the active profile's
`package.json` under `dsh.profile.bundles`. A copied profile patch that adds the
same `codex-audit` entry a second time can interfere with startup and reload.
Restart the service through its existing owner after installation.

Open a DSH session in a Git worktree with a commit. In the audit panel, open
**连接与运行环境** and click **检查本机条件**. This checks the session's Git
worktree, scratch-directory creation, provider configuration and required CLI flags without making
a model request. Codex runs on the DSH host. If the service has a different PATH
from your terminal, set the absolute path to Codex in the panel. On Windows
prefer the native `codex.exe` over a package-manager `.cmd` shim. A browser or
VS Code client on another machine cannot supply the host's executable, local
workspace path or authentication. When migrating settings, clear or update any
saved Codex path from the previous host.

Authenticate Codex as the account that runs DSH and run one manual audit to
verify the selected model and reasoning effort. Codex reads its own auth state,
with optional `CODEX_HOME` in the DSH process environment. A relative
`CODEX_HOME` resolves from the host's working directory before Codex starts
in the snapshot. The model selector
uses that account's available local catalog when present. Without a usable
catalog the plugin only lists its configured default, whose availability must
still be verified on that account. DSH's own model/provider credentials are not
used by Codex. Copying NDNSF, TPS Window, another host's profile state, or Codex
credentials from another machine is unnecessary.

For a custom gateway, configure `model_provider` and the matching
`[model_providers.<id>]` table in the host's Codex config. Supply its Responses
API `base_url` and use `env_key` for an API key. Set the named variable in the
environment that launches the DSH service. Environment-backed headers are also
supported. Static tokens and headers, command-backed credentials and custom
gateways that reuse the built-in OpenAI login are rejected with a configuration
error. Choose a provider you authorize to receive the workspace audit inputs.

## Development verification

This directory can be copied or cloned on its own. Use Node 22 or newer, npm
10 or newer, and Git. From a clean copy of this directory, run:

```sh
npm ci --ignore-scripts
npm test
npm pack --ignore-scripts --pack-destination /path/to/artifacts
```

The host supplies Agent, query, storage and gateway services. Its Web loader
supplies React, React DOM and the DSH Markdown renderer. The tests use
isolated fixtures and make no model requests.

Reply collection materializes each session observation once and indexes report
admissions before scanning assistant messages. It must not search the complete
history inside that loop: long sessions otherwise block the DSH event loop and
make the Web GUI appear disconnected. The long-history regression also preserves
delivery ordering, explicit reply correlation, and observation cleanup on errors.

The current native tests cover scheduling, ownership, cancellation, durable report
handling, restart state, real subprocess deadlines and stdin, event completion,
document admission and conflict protection, installed DSH service/decorator
contracts, steer delivery during a running agent, acknowledgement correlation,
and real Git working-tree snapshot contents. Fake agent tests do not prove live
DSH transport or a successful model invocation. Those are separate installation
acceptance checks.
