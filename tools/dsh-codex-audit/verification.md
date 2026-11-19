# Verification: dsh-codex-audit

## 0.1.17 migrate the pinned selection for existing sessions (2026-11-18)

- An existing Web session still persisted `gpt-6.1-sol` after 0.1.16 was installed.
  The manager used the default only for new sessions; the runner passed the saved
  selection to `codex -m`. Opening the manager now updates saved session model and
  effort to the pinned values. Settings unrelated to model selection and model
  evidence in completed records are preserved.
- All 78 tests passed. The migration test covers the saved settings, the next
  audit's runner inputs, and the prior record. Replayed the migration in memory
  with the installed module and the actual storage shape; the saved session became
  `gpt-6.2-sol / xhigh` and completed history was identical.
- Installed 0.1.17 into the Web profile with the official offline plugin command.
  After installation, the live `codexAudit/view` still reported `gpt-6.1-sol` and
  an active DSH goal. The running service was left in place. Live startup migration
  and a real audit with the new model remain unverified.
- Verified the served prompt dialog in an isolated Chrome profile with caching
  disabled, against the existing session on the live Web service. A real click
  on 立即审计 opened a visible centred dialog mounted under `document.body`.
  The prompt was prefilled, all four actions were present, and 取消 dismissed
  the dialog. The test did not click 开始审计. Existing tabs were not inspected.

## 0.1.16 the prompt dialog is a sibling portal, not a child of the panel body (2026-11-18)

- 0.1.15 made the dialog a portal, but the expression still sat in the children of `ca-body`, so the
  React element tree passed it as a child of the scrolling body. A portal's DOM parent is its second
  argument, so the DOM was correct -- but its React parent was still the body, which the panel
  re-renders on every state change. The block is now a sibling of the panel's own portal at the top of
  the returned fragment.
- Reported symptom after 0.1.15: the text "本次审计提示词" appeared inline in the panel between
  "恢复历史" and "连接与运行环境", i.e. exactly where it was in 0.1.14. That places it as the earlier
  layout, not the overlay: it was still being rendered as a body child.
- `test/client-guard.test.js` had selected the prompt dialog by `role="dialog"`, which also matches the
  panel itself; it now selects by `aria-label="本次审计提示词"`. That wrong selector is why the 0.1.15
  test passed while the dialog was in the wrong place.
- `npm test`: 77 passed, 0 failed.
- Not verified: still no real click in the Web GUI. The harness proves the element tree and the
  handlers, not the rendered position.

# Verification: dsh-codex-audit

## 0.1.15 the prompt dialog is a real overlay (2026-11-17; source only, not activated)

- 0.1.14 put the prompt panel inside the scrolling body, after the guard card and the connection
  details. Clicking 立即审计 therefore did appear to do nothing unless the operator scrolled down --
  reported as "没有弹窗". It was rendered, just not visible.
- Two causes, both fixed. The card was in the document flow rather than an overlay, and it sat inside
  an ancestor with `backdrop-filter`, which makes that ancestor the containing block for
  `position: fixed`; a modal nested there would have been positioned against the panel instead of
  the viewport. The dialog is now its own `createPortal` to `document.body`, centred, with a fixed
  backdrop, and the panel is a `role="dialog" aria-modal` overlay.
- Verified with `test/prompt-dialog.test.js`, which drives the component with a persistent state store
  and a setter that re-renders, then clicks the real nodes: no prompt dialog before the click, one
  after, prefilled with the configured default, buttons 清除 / 恢复默认 / 取消 / 开始审计, and
  开始审计 issuing exactly one `control` call carrying only this run's prompt before closing.
  `npm test`: 77 passed, 0 failed.
- Not verified: still no real click in the Web GUI. The harness renders the tree and invokes the
  handlers; it does not prove layout, stacking against other surfaces, focus trapping or Escape
  handling.

## 0.1.14 per-run audit prompt (2026-11-17; source only, not activated)

- "立即审计" now opens a dialog showing the configured default prompt. It can be cleared, replaced
  with a prompt for that one audit, and started. The text is this audit's only: it is stored on the
  record (`promptSource: 'run'`), never written into the session settings, and is gone when the
  panel closes or the page reloads. A scheduled audit still uses the configured prompt.
- An empty box is treated as "changed my mind", not "audit with no instructions": the request omits
  the prompt and the configured default applies. Otherwise clearing the box would silently strip the
  standing instructions from one run.
- `npm test`: 76 passed, 0 failed, including two manager cases (override applied and not persisted;
  blank override falls back) and one client case (dialog renders, starts from the default, offers 清除
  and 开始审计).
- Not verified: no real audit ran with an override, and the dialog was asserted through the existing
  `vm`-rendered client harness, not by clicking in the Web GUI. That harness renders the component
  tree; it does not prove layout, focus behaviour or that the host accepts the extra `prompt` field
  over RPC.

## 0.1.13 audit selection raised to gpt-6.2-sol (2026-11-17; source only, not activated)

- The audit selection now lives in one place, `AUDIT_MODEL` / `AUDIT_EFFORT` in
  `lib/audit-manager.js`. Previously the literal appeared in the default settings, the guard and
  the error text, and the GUI caption in `lib/client.js` was a third copy -- so advancing the model
  needed four coordinated edits and the panel could display a model the audit was not selecting.
- `AUDIT_MODEL` is `gpt-6.2-sol`; `AUDIT_EFFORT` stays `xhigh`. The host's `~/.codex/config.toml`
  names `gpt-6.2-sol`, and 14 recorded turn contexts in `~/.codex/sessions` report that model, so
  the pinned name matches what this host can actually select.
- The panel caption is derived from the settings the host reports, with an effort label table,
  instead of a hard-coded `GPT-6.1-Sol · Extra high`.
- `npm test`: 73 passed, 0 failed.
- Not verified: no real audit ran against `gpt-6.2-sol`. `npm test` exercises the guard, the
  default, the caption and the turn-context reader with fixtures; it does not prove the account can
  select that model at `xhigh`, nor that a live audit's observed context matches. The 0.1.6
  acceptance entry below still records what `gpt-6.1-sol` did and must not be read as covering this
  selection. `~/.codex/config.toml` sets `model_reasoning_effort = "medium"` for interactive use;
  the plugin passes `xhigh` explicitly and does not read that value.

## 0.1.12 activation-bounded audit scope (2026-11-16; installed and activated)

- Every off/on captures the observed session cursor plus bounded source hashes.
  Configuration edits and skip preserve the period; full successful reviews
  advance to their capture cursor. Failed/cancelled/blocked reviews do not consume
  full coverage. Completing an old period cannot advance a newly enabled one.
- Manual no-new-work and disabled-timer reviews explicitly look back one configured
  interval. ISO and numeric event times are supported. A lookback without a source
  baseline reports file-change counts as unknown, not zero. Full history remains
  available as background, with separate focused transcript and change manifests.
- Prompt now follows the user goal and related contracts where present, prioritizes
  current work and actionable evidence, and rejects the inference that missing
  snapshot material/empty commits prove no implementation or validation. Only the
  exact previous default migrates; custom and historical dispatched prompts remain.
- UI shows activation, coverage point, focus period/counts and limitations. Baseline
  hashes stay out of GUI polling. Baseline-only capture creates no scratch copy;
  secrets/source-size exclusions and stable-capture admission remain in force.
- `/home/tianxing/.local/share/deepseek-harness/runtime/node-v24.22.2-linux-x64/bin/node
  tools/dsh-codex-audit/test/run.js`: **73 passed, zero failures/skips**. Includes
  real CodexAudit host enable→observe cursor→workspace snapshot→focused transcript→
  completion→public view with a temporary Git repository, without a model request.
  Regressions cover off/on, failure, blocked scope, capture-vs-completion cutoff,
  delayed configuration superseded by disable, shutdown, and old-period completion.
  The real client factory test covers scope text while preserving folded defaults.
- Checked `origin/KVCache@8027f6c6b5e9c339e809da24a85661c183cbb421`:
  no corresponding plugin implementation exists. This plugin remains untracked;
  unrelated project/index edits were preserved. Reviewed changed owner, snapshot,
  GUI, storage and actual DSH RPC paths; `git diff --check` passed.
- Archived with `npm pack --offline --ignore-scripts`; **13 files** byte-match
  source and the installed package. SHA256:
  `ee80e09dd97b311871c4518ecd7c32838886b4a9edf8f4547e14f59cc0832ede`.
  Durable archive is `~/.dsh/profiles/web/plugin-packages/dsh-codex-audit-0.1.12.tgz`.
  Installed using the official DSH CLI/offline pnpm and persistent archive path;
  no dependency on disposable `/tmp` packaging files remains.
- Activation used the authenticated local **public**
  `pluginManager/setPluginEnabled(include:codex-audit, true)` operation. No service
  restart, module-cache manipulation, paid audit or injected task message was used.
  RPC returned `application=applied`, no warnings. Independent `codexAudit/view`
  then showed the new `auditWindow` interface and migrated prompt. The existing
  goal ID/revision/phase/activation were unchanged, audit idle, no plugin error;
  periodic auditing remained disabled and blocked-goal guard remained enabled.
- Functional scope capture uses the real host fixture; a paid live Codex report
  and pixel-level browser review were not run. Existing DSH sessions/history and
  configured interval were retained. Packaging scratch is retired after verification.

## 0.1.10 manual/periodic deadline repair (2026-11-15; installed)

- Confirmed from the real retained records: manual audit
  `AUD-ba091031-f60c-44a1-b55b-634804650227` started at 15:59:57 CST and
  finished at 16:08:59; the unchanged old timer admitted scheduled audit
  `AUD-9b8167fc-65e9-4eaf-8c88-093c9048fbf7` at 16:44:13, cancelled at
  16:45:24. These were sequential audits, not simultaneous duplicate jobs.
- Root cause: `AuditManager.start` reserved one job but did not advance an
  enabled schedule for manual or blocked triggers. Existing in-flight job
  deduplication worked; an old future deadline could still start another audit.
- New audit admission now advances the enabled timer to start + interval.
  Joining an existing job does not advance it again; disabled timers stay
  disabled. Periodic missed-slot coalescing and the separate blocked-goal queue
  retain their existing ownership. History rows show the trigger category.
- Added four manager regressions; the original implementation failed three
  with the observed stale deadline/extra-record behavior. After the fix, all
  **59 tests passed**, including simultaneous tick/click and a long audit
  crossing another deadline. The real client factory test checks trigger text
  and the timer explanation. No paid audit or three-hour wall-clock wait was
  used as a substitute for these deterministic checks.
- Inspected `origin/KVCache` at
  `8027f6c6b5e9c339e809da24a85661c183cbb421`; it has no corresponding
  `tools/dsh-codex-audit` implementation or regression to port. The plugin is
  currently untracked in this checkout; unrelated pending changes were retained.
- Package SHA256:
  `8491409f0fad84e312368d2c8c952f45af92e5766c606ee67543df4f65061b56`.
  All 12 packaged files matched source and the installed web-profile package.
  Installation used the official DSH plugin CLI with offline pnpm. The running
  instance was independently confirmed to retain the old start function;
  installation alone was not treated as activation.
- Activation waits for a normal `completed` turn boundary, all agents idle,
  empty inboxes and no audit. A bounded one-shot listener uses only public
  Cordis events, native `goals.pause` and session flush; it does not replace
  services, module caches or fibers, and never cancels a running turn. The
  user previously explicitly authorized a safe-window restart and restoration
  of this same goal. Final activation evidence will be appended below.

## 0.1.9 blocked-goal review (2026-11-15; installed and activated)

- User-authorized behavior: a native goal becoming blocked triggers an immediate
  independent audit, produces a repair plan, delivers it to DSH and resumes the
  same goal when the plan is actionable. Public downloads, compatible database
  selection/configuration, dependency setup and resource alternatives are choices
  the auditor can make. DSH executes and verifies the plan. Genuinely unavailable
  private credentials, payment authorization or user-only information remain
  waiting cases. Existing permissions, acceptance requirements and explicit
  resource limits are retained.
- `node tools/dsh-codex-audit/test/run.js`: **55 passed**. Includes immediate
  block-event dispatch while periodic auditing is off, capacity queueing,
  deduplication, report admission before native resume, actionable downloads and
  database setup, waiting decisions, stale sources, round limits, changed/paused
  goals, human-stop veto, bounded source-capture retries and repeated-block caps.
  The component test executes the real client factory with a React hook fixture,
  verifies its collapsed default, blocked progress and revision-bound retry RPC.
  Native scheduling/recovery tests and previous audit tests also passed.
- Archive `dsh-codex-audit-0.1.9.tgz`, SHA256
  `3256f8b72e60c9312f6fc7f573fbbf46d0bf091b59a941dacab2e5985add9a4e`.
  All 12 packaged files matched source and the installed web-profile package.
  Installed using DSH's existing plugin CLI and isolated Node/pnpm, without
  modifying other plugins or the default Node runtime. An intermediate 0.1.8
  archive was superseded before activation by this clarified decision policy.
- Initial activation was deferred. The running DSH goal was active/armed at
  round 23 with work and input pending. No whole-service restart was performed.
  A native replacement preview reported only `codex-audit` and six own modules;
  native partial reload returned without replacing its old instance. Read-only
  checks still showed the original fiber and no guard. A proposed manual
  cache/registry/fiber replacement was rejected by automatic approval review for
  risk to the live goal/session; it was not executed.
- The user subsequently explicitly approved an idle-window restart and restoration
  of the same goal. After round 26 completed normally, a maintenance pause was
  recorded through native `goals.pause` and flushed. All agents were idle, no
  non-goal input or audit was pending, and the exact paused goal revision was
  rechecked through public RPC. DSH exited gracefully on SIGTERM; no force kill,
  module/registry manipulation or active-command interruption was used.
- Restarted through the original Node 24 launcher and web profile. Restored the
  original `open-design / deepseek-v4.1-flash` selection. Public `goals/resume`
  resumed goal `goal-ff447d2b-6e07-45ab-8407-35649026b4f3`; revision changed from
  2 to 3 for the pause and 4 for the explicit resume. The native driver admitted
  round 27 while retaining the 256-round cap. Public `codexAudit/control` enabled
  the guard for that exact goal. Independent RPC checks confirmed guard enabled,
  guard status running, goal active/armed, DSH running, no plugin error and the
  original enabled three-hour periodic audit setting. Completed at
  `2026-11-15T20:37:52.819Z`. See `activation-20261115.json`.
- No real blocked-goal Codex audit/automatic unblock has been claimed
  from these fixture tests. No product build, Spec edit, product-source change,
  commit or push was performed. User conversation and active NDNSF work were
  retained. `origin/KVCache` at
  `8027f6c6b5e9c339e809da24a85661c183cbb421` has no corresponding plugin source.

## 0.1.7 working-tree audit update (2026-11-15)

- Each invocation still copies actual working files, and now explicitly records
  `sourceMode=working-tree`, capture time and selected-source staged/unstaged/
  untracked/deleted path status. Prompt, delivery and history distinguish the
  working tree from its HEAD baseline. Legacy records do not invent this metadata.
- Admission rechecks both path status and complete admitted source contents/list.
  Completion retains the existing stale-source warning for work performed by DSH
  after capture. Historical conversation cannot override current snapshot files.
- `node tools/dsh-codex-audit/test/run.js`: 32 tests passed. Snapshot fixtures
  exercise staged-only changes, staged content subsequently edited again,
  deletions, untracked source, ignored active Spec, secret exclusions, source
  staleness and fresh recapture at unchanged HEAD. Real host `steer()` fixture
  asserts working-tree language in the delivered report.
- Package `dsh-codex-audit-0.1.7.tgz`, SHA256
  `ced0c96fbcd2862432d5e45bff887360c391e069ea9256f77287941236dd985b`;
  all 11 packaged files matched source and the installed web-profile package.
  Installed through DSH's maintained plugin CLI using its isolated Node 24/pnpm.
- An attempted HMR reconciliation was followed by loss of the Inspector endpoint
  and DSH process. Its async result could not be retrieved (invalid remote object
  ID); no successful hot replacement is claimed. Restored the original web profile
  with the existing DSH launcher on loopback port 3080. The restored service
  returned its expected unauthenticated HTTP 401, and Inspector script-source
  checks confirmed both loaded `index.js` and `snapshot.js` contain the new
  `describeBasis` implementation. A disconnected VS Code panel may need to reconnect.
- No new paid Codex audit was launched, and no claim is made that an interrupted
  DSH model request resumed automatically. No product build, product code edit,
  commit or push was performed. `origin/KVCache` at
  `8027f6c6b5e9c339e809da24a85661c183cbb421` has no corresponding plugin path.

## Historical 0.1.6 verification

Verified locally on 2026-11-15, using the existing DSH web profile and isolated
Node 24. Original DSH work continued throughout. No NDNSF product build, cluster
job or product-source edit was performed by this plugin task.

## Frozen delivery

- Source: `tools/dsh-codex-audit/`; installed version: **0.1.6**.
- Archive: `~/.dsh/profiles/web/plugin-packages/dsh-codex-audit-0.1.6.tgz`.
- SHA256: `ac15c03d9e57a29a8138572eded600c81ce8df87e6467fd610bff325b6ad82c8`.
- All eleven packaged files matched source, archive and installed bytes.
- Backup: `~/.dsh/backups/before-codex-audit-20261115T152352Z`.
- `origin/KVCache` at `8027f6c6b5e9c339e809da24a85661c183cbb421` contains no
  corresponding `tools/dsh-codex-audit` implementation to port.

## Acceptance evidence

| Requirement | Actual verification |
| --- | --- |
| Parallel execution | Real Codex process ran while original DSH agent remained running. |
| Model/effort | Explicit arguments and exact matching Codex turn contexts confirmed `gpt-6.1-sol` / `xhigh`; no fallback. Version 0.1.6 backfilled both completed audits from matching thread IDs. Real GUI displayed `Codex 运行记录确认：gpt-6.1-sol · Extra high（xhigh）`, removed the unconfirmed row and remained collapsed. This confirms Codex runtime selection, not a remote server's internal routing. |
| Host schedule and controls | Core due-time/restart tests; real GUI 1 → 5 → 3 hours, enable switch, skip 12:39 → 15:39, then restore three hours. Final schedule enabled at three hours. No wall-clock three-hour wait claimed. |
| Snapshot | Real Spec 224 audit: HEAD `187f6d016396cfac21196f6bc3325239d16c7d57`, seq 63869, 2725 files / 76838596 bytes, 37787206 transcript bytes in 146 hashed chunks. |
| Read-only | Read-only Codex sandbox and isolated configuration; audit independently checked snapshot hashes unchanged. Fixture source, instructions and contract also unchanged. |
| Report and actual response | `AUD-0be93e15-b012-4716-aae4-8a0d868dd529`, Codex thread `01a1dfda-7d78-7bb2-8a53-540d80a5c762`: 5181-character report admitted at original DSH seq 63964; explicit DSH response `71bc71f4-6a73-4c7c-9974-ce6744dced17`. |
| Document correction | Real isolated audit `AUD-0b6e655e-e316-4a6b-90b5-b10caa03d404` proposed README's deliberate `add(1,2)=4` → `3`. Plugin applied it. arithmetic.cpp, AGENTS.md, feature pointer and Spec bytes stayed unchanged. |
| Lost-input recovery | Fixture initially lacked a DSH model; report left inbox before admission. After selecting the existing main route for that fixture, the production reconciler resent the same identity: admitted seq 24, two delivery attempts, explicit response `16413cb9-8344-4a13-93bf-b8ed6b8cc6e9`. No second Codex audit. |
| Actionable errors | Real GUI invalid Codex path displayed `CODEX_NOT_FOUND`, path and remedy; automatic discovery restored. Core tests cover auth/model errors, bounds, timeout/cancel, storage and delivery failures. |
| Visual history | Final GUI paginates summaries, loads full reports on expansion, renders real Markdown tables and actual replies, shows token usage and successfully locates the original response in Chat. |
| Collapsed GUI | Starts closed; click opens; body Portal has 744px panel height in 776px viewport; advanced settings folded; Esc closes; hidden/visible transitions leave it collapsed. Left closed. |
| Persistence | Real plugin reloads retained reports, failures, replies and settings through the storage-domain facility. Core restart tests mark interrupted work. Host/OS power loss not separately exercised. |

`node test/run.js` with isolated DSH Node 24: **31 tests passed**. They include
real subprocesses, real temporary Git snapshots, installed DSH service contracts,
reply chronology, dropped-input recovery, conflicts and bounded retry behavior.
Metadata tests cover exact thread identity, differing local config, missing
evidence, mixed selection, path traversal and excluding later thread resumes.

## Review and scope

Review corrected packaging dependencies, storage-domain naming, repeated stream
capture, user-event deduplication, persistence before delivery, reply chronology,
full-report polling, stale-source checks, output/retry bounds, invalid proposals,
guarded document writes, log-admission recovery and GUI clipping/default state.
Same-version package cache reuse was fixed through immutable version increments
and source/archive/installed byte comparison. Initial failed attempts remain in
history instead of being presented as successes.

Spec 224 findings are static audit advice, not inference qualification. Its task
changes remain proposals and quickstart correction awaits review against current
work. Fixture acceptance qualifies plugin behavior only. Unsupported local Codex
conditions and custom provider configurations produce errors instead of downgrade.

Own fixture Agent was released through its exact owned lifecycle callback while
the original DSH agent remained running. Snapshot owners already removed their
directories. Closeout removes the owned acceptance directory, pack cache and
superseded local tarballs; durable reports, backup and final delivery remain.
## Clean-directory delivery check — 2026-11-18

Version `0.1.18` was tested from a copy containing only the Git-visible plugin
files and its new lockfile, with an empty HOME and no inherited DSH, Codex or
Node module paths. `npm ci --ignore-scripts` and `npm test` passed 79/79,
including standalone CLI discovery. `npm pack --ignore-scripts` produced
`dsh-codex-audit-0.1.18.tgz` (SHA-256
`da5d9da2e2d42cf5931cd3f786b793162611f2aba0d06946fb987ccdcf5f5e6b`).
Installed that tarball alone with `dsh plugin --profile web add` in a fresh DSH
`0.2.2-rc.1` Web profile. An authenticated request to `codexAudit/view` on the
loopback Host returned disabled settings and empty history for a new session.

## 0.1.19 selectable audit reasoning effort (2026-11-18)

The local Codex model catalog lists all four offered levels for
`gpt-6.2-sol`. The session setting defaults to `xhigh` and preserves valid
saved selections when the Host opens. Changing only the effort preserves the
audit deadline and coverage period.

Node 24 passed 81/81 tests, including the rendered selector, Host control,
storage reopen, invalid selection, and the actual arguments received by a
fixture Codex executable for each of the four choices. Nested Node stdout was
empty under the default tool sandbox (`EPERM` in a direct spawn probe). The
same suite passed on the host with fixture-only model execution.

The exact tarball was installed and started in a clean DSH Web profile.
Its authenticated RPC accepted the four choices, rejected `max`, and
returned the previously selected `ultra` after a Host restart. No real
Codex audit was started.

SHA-256 `58cac33a12bba2b279f872930bd4667c7aadbd58589d102543b4365cb4d73efd`.
All 13 installed files in the local Web profile matched the archive.
The live service still had active DSH tasks at installation.
This check made no Codex model request and did not use the personal profile.
# Model and effort selection in 0.1.20 (2026-11-18)

- Node 24 plugin suite passed 82 tests. An isolated DSH Web profile installed
  the archive, switched models, applied the new model's compatible efforts,
  rejected an invalid combination, and retained the choice after restart.
  The runner's fixture verified Codex argument forwarding without a live audit.
- The production Web profile contains the verified package. Its current
  Host still serves the previous audit API and owns a running goal. Activation
  waits for an idle window because the plugin manager reconciles the whole profile.
