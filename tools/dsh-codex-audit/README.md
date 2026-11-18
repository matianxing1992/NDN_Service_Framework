# DSH Codex parallel audit

Development version. The host independently schedules read-only Codex audits while
DSH continues its task. The conversation composer provides an audit-history panel.
Default model/effort are `gpt-6.2-sol` / `xhigh`, defined once as `AUDIT_MODEL` / `AUDIT_EFFORT`;
no silent fallback is permitted.

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
Bounded scratch
snapshots live in owned directories on disk-backed `/tmp` and are removed after
the attempt. Credentials, model-sized assets, build outputs and symlinks are
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
Custom provider routes currently produce an explicit unsupported-configuration
error instead of silently using another route. Model and effort are confirmed
from the exact Codex thread's `turn_context` runtime records, bounded by this
audit's completion time. Existing history is backfilled on plugin initialization.
The UI identifies this evidence as Codex runtime confirmation; it does not claim
knowledge of a server's internal routing. Missing logs retain an explicit reason
instead of inventing a confirmation; contradictory runtime selection is an error.
The plugin does not control
the VSCode sidebar or promise its audit thread appears there.

Completed reports are saved before delivery to the original DSH session via
`steer()`. This does not cancel the current model request or tool. DSH reads the
report at its next model step. Stable message identities deduplicate inbox and
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

## Development verification

Use the Node runtime belonging to the DSH installation, keeping the system Node
unchanged. Resolve the declared runtime dependencies from that installation or an
isolated development environment, then run `node test/run.js`.

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
