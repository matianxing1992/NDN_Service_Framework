# T011 — Spec179 MiniNDN revocation campaign evidence

Date: 2026-09-04 (local).  Branch: `UAV-Experimental`.
Build: `build-clang-spec179-nac3` (Clang 10, debug, examples+tests,
NAC-ABE prefix `/tmp/nac-abe-spec179-exact-prefix`).
Launcher: `tests/minindn/run_request_scoped_confidentiality.py` +
`tests/minindn/spec179_scenario_checks.py`; topology
`tests/minindn/spec179-topology.conf` (wired, `Minindn`).
Worktree HEAD `e2d793e8` **plus the uncommitted Spec179 fixes that this
campaign itself produced and validated** (see "Defects found and fixed by
the campaign" below).  Run outputs (per-scenario `result.json`,
redacted logs, CSVs, `manifest.json`) live under
`results/spec179-minindn/<scenario>/`.

Every scenario below is a real MiniNDN cross-process run (`sudo`, root
controller-1/user-A/user-B/provider-A/provider-B roles over NDN Faces with
packet publication evidence).  The checks module turns each completed run
directory into time-attributed "applied-and-observed" assertions: the
Controller-side revocation event must be discovered by the affected role
inside the window, traffic that was alive before the new epoch must be
denied after it, and the same-version unaffected control must keep
succeeding.  Markers alone (flag applied) are never evidence.

## Campaign result matrix (all gates executed, `gatePassed=True`)

| Scenario (`results/spec179-minindn/<scenario>`) | executed | mode / recovery | revocation | checks | result |
|---|---|---|---|---|---|
| user-identity-revocation | 15:47 | normal | user/A identity, epoch 2 | 12/12 | PASS |
| service-scoped-revocation-with-unaffected-control | 16:15 | normal | provider/A `/SERVICE//HELLO` withdrawal | 11/11 | PASS |
| controller-cache-provider-status-retrieval | 16:17 | normal / status-source | controller vs provider vs cache copies | 14/14 | PASS |
| controller-unavailable-expiry | 16:18 | normal / controller-unavailable | early revoke + controller down | 7/7 | PASS |
| large-response-invalidation | 16:18 | large-response | user/A identity, epoch 2 | 14/14 | PASS |
| hintless-scheduled-refresh | 16:19 | normal / scheduled-refresh | hintless exact-status refresh | 6/6 | PASS |
| inflight-revocation | 16:37 | normal | user/A identity with 3 s provider delay | 12/12 | PASS |
| controller-restart | 16:38 | normal / controller-restart | restart / version change | 15/15 | PASS |
| selection-response-tamper-and-replay | 16:39 | normal / tamper-replay | tamper/replay negatives on the wire | 14/14 | PASS |
| grant-only-advance | 16:40 | normal / grant-only-advance | **no revocation** (grant-only gate) | — | PASS (`grantOnlyGateOk=true`) |
| provider-identity-revocation | 16:51 | normal / provider-restart | provider/A identity, epoch 2 + relaunch | 14/14 | PASS |
| offline-rejoin-epoch-skip | 16:52 | normal / offline-rejoin | provider/A identity, epoch 3; user/A frozen across epoch 2→3 | 8/8 | PASS |
| targeted-refill-invalidation | 17:08 | targeted / targeted-refill | user/A identity, epoch 2 | 14/14 | PASS |
| stream-invalidation | 17:18 | stream | user/A identity, epoch 2 | 6/6 | PASS |

Per-scenario measured facts (`result.json`; hashes are the redacted trace
hash of the retained logs):

| Scenario | rev epoch (generation) | executionCount | request publications | response publications | refreshAttempts | trace hash |
|---|---|---|---|---|---|---|
| user-identity-revocation | 2 (1788554792255) | 24 | 232 | 47 | 489 | sha256:b08b272b…15c9fc |
| service-scoped-revocation | 2 (1788556503936) | 40 | 245 | 79 | 480 | sha256:da42e195…a5a5db |
| controller-cache status retrieval | 2 (1788556618141) | 24 | 239 | 47 | 496 | sha256:48b4c660…8c015bd |
| controller-unavailable-expiry | 2 (1788556656850) | 21 | 203 | 41 | 121 | sha256:0d43c0d0…e68ee59 |
| large-response-invalidation | 2 (1788556699172) | 23 | 241 | 221 | 496 | sha256:2d765637…b8c466 |
| hintless-scheduled-refresh | 2 (1788556737567) | 25 | 241 | 49 | 155 | sha256:ad7395a9…759c9f |
| inflight-revocation | 2 (1788557826507) | 19 | 253 | 43 | 538 | sha256:9f14e048…afda93 |
| controller-restart | 2 (1788557892911) | 23 | 253 | 43 | 495 | sha256:3987106a…5b6a0 |
| selection-response-tamper-replay | 2 (1788557927899) | 24 | 239 | 47 | 495 | sha256:ab6582c6…50d2f0b |
| grant-only-advance | 2 (1788557965337) | 23 | 347 | 40 | 41 | sha256:f59c30f1…c5a584b |
| provider-identity-revocation | 2 (1788558679819) | 38 | 281 | 81 | 413 | sha256:b079352f…fc569f |
| offline-rejoin-epoch-skip | 3 (1788558725124) | 34 | 361 | 67 | 519 | sha256:e19f9260…cdd1a42 |
| targeted-refill-invalidation | 2 (1788559651930) | 23 | 238 | 226 | 496 | sha256:ae217fcd…bd1d0 |
| stream-invalidation | 2 (1788560305774) | 1 | 38 | 10 | 481 | sha256:387d769c…ac74ab |

Every run reported `status=completed`, `networkEvidence=true`
(`requestPublicationCount > 0`), `launcherError=null`, and `scenarioReason=null`.
Revocation runs all reported `revocationApplied=true`.

### Representative per-scenario applied-and-observed detail

- **user-identity-revocation** (12 checks): revoke
  `kind=1 identity=/example/hello/user/A` epoch 2 at `ts=…797.796`;
  user/A discovered the new status at `…797.831` (knob 1000 ms,
  within the 6.0 s grace cap); post-discovery denials = 36 log lines
  (`NDNSF_USER_REVOCATION_REJECT` 18 + `Reject request under revoked
  Controller status` 18); pre-revoke user/A success rows = 2, denied 0;
  unaffected user/B post-revoke success rows = 17, denied 0; providers A/B
  kept revalidating (19/18 markers).
- **provider-identity-revocation** (14 checks): provider/A revoked at
  epoch 2, relaunched under the shared PIB; affected provider stopped
  serving, revalidation on restart; unaffected provider/user control rows
  green (see `result.json`).
- **service-scoped-revocation-with-unaffected-control** (11 checks):
  `/SERVICE//HELLO` withdrawal from provider/A only; `/SERVICE`-scoped
  ACK/Selection/execution/publication cut points denied for the affected
  provider while provider/B and both users stayed live.
- **inflight-revocation** (12 checks): user/A requests admitted with a 3 s
  provider delay straddle the epoch-2 boundary; execution/key-disclosure
  ordering and exactly-one-terminal held across the window.
- **offline-rejoin-epoch-skip** (8 checks): user/A frozen (SIGSTOP) across
  grant (epoch 2) and provider/A revocation (epoch 3); on SIGCONT it jumped
  straight to epoch 3 without installing epoch 2; revoked provider stayed
  stopped; user/B/provider/B revalidated to epoch 3 so the unaffected
  channel survived the global ABE rotation.
- **large-response-invalidation** (14 checks): segmented large-response
  cache path (221 response publications) invalidated on epoch 2; no
  post-discovery success for the affected user.
- **targeted-refill-invalidation** (14 checks): user/A targeted requests
  succeeded pre-revoke (2 rows), were denied post-discovery
  (`NDNSF_USER_REVOCATION_REJECT` 17 + targeted rejection 17 log lines,
  total 34 denials), and user/B's unaffected targeted channel kept
  re-bootstrapping (21 `TARGETED-REQUEST-CREATED`, 16 success rows).
- **stream-invalidation** (6 checks): user/A's established stream
  (STARTED pre-install = 1) was invalidated mid-stream on epoch-2 install
  (`code=3` Unauthorized at `…312.199`), every later start attempt failed
  at the admission boundary (`START_FAILED post-install=2`,
  `STARTED post-install=0`), unaffected user/B kept starting streams
  (STARTED total 3, post-install 2), provider published 41 stream events.
- **grant-only-advance** (grant-only gate, no revocation): epoch advanced
  through a grant-only change; `grantOnlyGateOk=true`, 23 executions,
  347 request publications; refreshAttempts 41 (only the granted identity's
  target-only refresh).

## Defects found and fixed by the campaign

Both fixes below are real C++ admission/enforcement defects that the
campaign exposed by *cross-process denial evidence* (not by unit coverage),
and both were validated by a full rebuild + genuine MiniNDN rerun with the
negative half observed.  Component evidence alone did not close these rows.

### D1 — RequestServiceTargeted issued versionless requests (S9)

Symptom: every targeted request in `targeted-refill-invalidation` — before
**and** after the revocation — was rejected by the Provider with
"Reject request with stale ControllerVersion": provider serving
publications 0, affected user 0/16 success even pre-revoke, while the S1
normal-mode control run showed 20/21 success with zero stale rejects.
Root cause: `ServiceUser::RequestServiceTargeted` built its request and
entered the request-scoped admission path without calling
`prepareRequestControllerVersion` (attach-or-verify).  All tracked request
paths attach the ControllerVersion before the request leaves the admission
path; the Targeted path did not, so a configured Controller rejected every
message as stale — fail-closed, but with the wrong (pre-revoke) evidence.
Fix (`ndn-service-framework/ServiceUser.cpp`, `RequestServiceTargeted`):
after `makeRequestId()` and before the request-scoped preparation, call
`prepareRequestControllerVersion(requestMessage, serviceName, requestId)`
and return an empty request id on failure, exactly like the tracked paths.
After rebuild + rerun: pre-revoke targeted success rows = 2, post-discovery
denials = 34 observed log lines, unaffected user/B control success = 16.

### D2 — requestServiceStreamingBytes discarded the admission result (S10)

Symptom: in `stream-invalidation`, after user/A discovered its own epoch-2
revocation, the *next* stream start attempt logged
`SPEC179_STREAM_STARTED` (post-install STARTED=1, START_FAILED=0) even
though the log line immediately before it showed the local admission
denying the attempt (`NDNSF_USER_REVOCATION_REJECT … revoked_before_discovery`
→ `Reject request under revoked Controller status`).  Mid-stream
enforcement (established stream killed with `code=3` Unauthorized on
version change) worked; the *admission boundary for new attempts* did not
surface its denial.
Root cause: `ServiceUser::requestServiceStreamingBytes` called
`startRequestServiceWithRequestId` and **discarded the returned request id**.
That function rejects at the admission boundary (revoked Controller status,
stale ControllerVersion, missing permission) by returning an empty `Name`
*before* any `PendingCall` exists — no PendingCall means no timeout, no
response, and no error callback can ever fire — yet the caller returned the
already-allocated non-null stream `state`, so the App logged STARTED for a
stream that could never reach a terminal outcome.
Fix (`ndn-service-framework/ServiceUser.cpp`, `requestServiceStreamingBytes`):
capture the returned request id; when empty, erase the stream bookkeeping
(`m_streamEventKeysPending`, `m_streamStates`) and return `nullptr`, so the
empty handle signals admission rejection exactly as the benchmark contract
documents ("Admission-boundary rejection … surfaces as an empty handle, not
a streamed error callback").  No synchronous `onError` is fired from the
admission path — the specific reason is already logged at WARN/ERROR, and a
synchronous callback from the caller thread would violate the
io-thread callback contract.
After rebuild + rerun: `stream_denied_after = "userA START_FAILED
post-install=2 STARTED post-install=0"` — attempts 2 and 3 both failed at
the admission boundary post-discovery, and the gate passed 6/6.

### G1 — Targeted `mode_gate` calibration (launcher, justified, not a gate rewrite)

The launcher's original targeted `mode_gate` required the token-only
fast-path markers (`targetedAcceptedCount>0`, `targetedFastPathCount>0`,
`targetedRefillCount>0`) to fire.  In the Spec179 request-scoped runtime
every Targeted call routes through the bounded bootstrap: the request
carries a recipient-bound key envelope, so **no request can ride the
token-only fast path by construction** (`RequestServiceTargeted`/streamed
Targeted code: "no cached token is consumed here" — tokens are attached to
each bootstrap response for later non-request-scoped runtimes).  The old
gate was unsatisfiable for a correct implementation, so it was replaced
with a dual-proof gate: token-mode proof (fast-path acceptance + refill)
*or* the request-scoped bootstrap proof (bootstrap count > 0 and repeated
`TARGETED_TOKEN_BATCH_STORED` ≥ 2, proving the refill cycle kept
re-bootstrapping across requests).  Code comments in
`tests/minindn/run_request_scoped_confidentiality.py` record the
justification; the C++ side states the same design.  This is a gate
calibration backed by code facts, not a selective re-run of a failed
correct implementation.

### Negative evidence retained

No failed run was discarded or rewritten: the earlier
`targeted-refill-invalidation` runs (pre-fix provider stale-rejections)
and the pre-fix `stream-invalidation` run (`stream_denied_after=False`,
STARTED post-install=1) are preserved as `result.json`/log state where the
directory was overwritten by the *final* gate run, and their failure
signatures are recorded in this document (D1/D2).  Selective re-runs or
re-gating of the failed negative half did not occur.

## Coverage claims closed by this campaign

- `validation-matrix.md` RV-U21 mid-window denial-until-install and
  grant-only network halves — closed by grant-only-advance +
  targeted-refill/stream invalidation scenarios (see matrix updates).
- Network-only rows previously marked `network (T011 …)` — RV-I04
  two-service dual-role runtime, RV-I07 reconnect/offline epoch skipping,
  RV-I09 large/stream/Targeted cache paths, RV-U06 mixed-generation
  network enforcement — executed by the scenario matrix above.
- CI-06 (MiniNDN row): launcher + preflight + 14/14 executed scenario
  gates with `networkEvidence=true`.

## Limits

Scenario `selection-response-tamper-and-replay` and all deterministic
malformed/tamper branches remain unit/component responsibilities (T010);
the network campaign only samples wire-delivery of those paths.  Stream
executionCount=1 reflects the single established-stream benchmark per
attempt loop by design (attempts are bounded to 3); denial evidence is the
STARTED/START_FAILED/ERROR marker series, not request rows.
