# Online authorization audit and MiniNDN repair

Scope: online grant/revoke correctness on `UAV-Experimental`, starting at
`d2833215`. T017 covers Controller recovery; T018 covers same-process online
grant, startup readiness, error reporting and truthful network evidence; T019
covers dependency callbacks crossing invalidation and OpenABE error reporting.

**Current verdict: PASS for this local online-authorization repair scope.**
NDNSF unit182/182 and integration72/72, NAC dependency14/14, launcher15/15 and
one complete MiniNDN campaign16/16 pass. GDB/NDN_LOG and retained negative runs
support the repairs. T017–T019 are closed; T014 upstream dependency publication
remains external. This supersedes the earlier runtime-grant/revoke verdict;
it is not an upstream release or TigerCluster qualification claim.

## Findings and repairs

| ID | Severity | Reproduced problem | Repair |
|---|---|---|---|
| OA-1 | HIGH | Same-target revoke retry returned before recovery; grant bypassed pending rotation and removed the withdrawal. | Reconcile before duplicate handling and any grant mutation. Failed recovery retains the withdrawal; successful regrant uses current fresh ABE material. |
| OA-2 | HIGH | Recovery changed ABE parameters at an already-published ControllerVersion; real RevocationState rejected the conflicting status. | Persist a newer epoch before every recovery attempt. Completed duplicate revokes remain no-ops. |
| OA-3 | HIGH, evidence | Grant collector dropped failed rows using the earliest Provider log as a cutoff; control failures were absent from gate conditions. | Retain every terminal target/control row, expose both failure counts, require zero failures in the grant probes. Reprocessed negatives stay negative. |
| OA-4 | MEDIUM | CLI returned zero for completed runs whose gate failed. | Require explicit gatePassed=true; campaign propagates failure, validates scenario names and refuses overwritten evidence. |
| OA-5 | MEDIUM, coverage | First-grant probe relied on constructor blocking. Empty permission responses did not exercise retry exhaustion. | Explicit App renewal in both grant probes; isolated User/B transport loss until observed final timeout in the late case, removed in finally. Record both fault boundaries. |
| OA-6 | MEDIUM | Milliseconds were supplied to the seconds-based open-loop duration; count did not bound this workload. | Convert units at the CLI boundary and allocate independent workload/lifetime windows. |
| OA-7 | HIGH | Unprovisioned User and Provider constructors waited indefinitely for their first DKEY, preventing App renewal. | Start existing Consumer bootstrap asynchronously and return; retain authorization enforcement. |
| OA-8 | MEDIUM | One-second drain truncated valid three-second delayed responses with five-second request timeouts. | Six-second drain and adequate role lifetime; retain incomplete/failed rows. |
| OA-9 | HIGH | After asynchronous startup, valid permission/status admitted Requests before the first DKEY installed. ACKs arrived but could not decrypt. | Real User admission explicitly requires initial Consumer readiness. LocalMock fixtures retain their explicit crypto boundary. |
| OA-10 | MEDIUM | Both hybrid decrypt methods moved onError into the success closure, leaving unwrap-error handling empty. | Copy the shared callback into success/unwrap paths and retain synchronous exception reporting. |
| OA-11 | MEDIUM, harness | Faster startup made timed SIGCONT precede the real epoch-3 revoke; the offline role observed epoch 2. | Wait for the actual Controller revoke marker before resuming the role. |
| OA-12 | MEDIUM, harness | Late grant target succeeded21/21 but control exited before traffic with a missing-signing-certificate error; the public key/certificate remained in the shared PIB. Concurrent read/write regression reproduces rollback-journal reader lockout; ndn-cxx treats non-ROW as absent. | Initialize only the campaign PIB in WAL mode before roles start, retain writer serialization, and record the mode. The App did not expose the nested SQLite code, so contention attribution is an inference supported by the reproduced mechanism. |
| OA-13 | HIGH | Consumer content/CK callbacks lack cache-generation checks. Late content reaches crypto after DKEY clearing; late CK repopulates invalidated state. | T019: fence both stages before mutation/callback. Real segmented regression red5/23 failed; unchanged green23/23. |
| OA-14 | HIGH, availability | Provider/A aborts with uncaught `oabe::_OpenABE_ERROR` after epoch3 installation; ABESupport only converts ZCryptoBoxException. | T019: normalize enum errors into NacAlgoError. Invalid CP/KP input red2/4 failed, unchanged green4/4 including valid-key recovery. GDB confirms the controlled old-content reproduction reaches OpenABE importUserKey with a cleared DKEY; no stack from the original network process is claimed. |

The normal grant probe uses the existing **250ms** status-refresh setting with
1rps traffic; the late grant uses **1s**. Distributed exact-version convergence
is distinct from DKEY fan-out. Prior runs with 1s/no forced refresh include
transition timeouts and remain recorded below. No zero-interruption guarantee
under default refresh timing is claimed. No deadline or success filter is
relaxed to hide an unsuccessful request.

## Executed red/green evidence

All relative run paths below are under
`results/spec179-online-auth-20260905/`. Generated logs, keys, binaries and
results are retained locally and excluded from Git.

| Gate | Executed result / boundary |
|---|---|
| Controller recovery reproduction | Retained `gates/integration-before-fix`; `gates/controller-red.log`: exit201, 10 failures. |
| Controller repair | `PendingRotationFencesGrantAndPreservesImmutableStatus`: 36/36 assertions, exit0, `gates/controller-green.log`. Three independent retry/regrant/reconcile legs, immutable accepted statuses, retained-old-DKEY rejection on fresh ciphertext and replacement-key success. |
| Real asynchronous constructors | `UnprovisionedRuntimesConstructAndRemainUnauthorized`: 8/8, five-second bound, `gates/bootstrap-constructor-green.log`. No DKEY/status, zero User publication and zero registered Provider execution. The old constructor hang is reproduced by MiniNDN, not by a claimed pre-fix run of this added case. |
| First-DKEY admission and callback red | `OnlineGrantWaitsForInitialDkeyAndReportsUnwrapFailure`: 4/8 assertions failed, exit201, `gates/readiness-red.log`. Valid permission/status still produced one Request; actual Consumer missing-key error invoked zero callbacks. Retained `gates/integration-before-readiness-fix`. |
| First-DKEY first repair | `gates/readiness-green.log`:6/8 passed. Error callbacks recovered, but the base RequestMessage entry bypassed the helper and still published. This is a failed gate. |
| First-DKEY base-entry repair green | The shared admission helper now also guards `startRequestServiceWithRequestId`. Unchanged focused regression8/8, exit0, `gates/readiness-base-green.log`. |
| Final-candidate unit gate | Unit182/182,11971 assertions, exit0, `gates/unit-base-final.log`, after the base-entry repair. |
| Final-candidate integration gate | Integration72/72,1278 assertions, exit0, `gates/integration-base-final.log`, run after compilation without concurrent build load. |
| Expanded C++ gates before final readiness repair (historical) | Unit182/182,11971 assertions (`gates/unit-final.log`); integration71/71,1270 assertions (`gates/integration-final-isolated.log`). Superseded by the final T019 NDNSF gates below. |
| Timing-sensitive gate failure retained | Concurrent App compilation produced stream retryCount2 instead of1, integration70/71 (`gates/integration-final.log`). After compilation, focused12/12 and full71/71 passed without weakening an assertion. CPU scheduling is a plausible cause, not a separately controlled load experiment. |
| Launcher regressions | Initial target-row/CLI regressions failed before repair. New control-retention regression also failed before repair. Current14/14 pass (`gates/harness-readiness.log`), including refusal to apply the transport fault in the host namespace. |
| Stronger control reanalysis | `gates/grant-control-reanalysis.log`: retained late run target21/21/control60/60; negative normal run target12/13/control12/24. All twelve failed control rows remain counted. |
| Shared PIB contention red | `gates/pib-concurrency-red.log`: new concurrent signing-identity reader fails with `database is locked` while another role holds a write transaction. The test uses the launcher's actual PIB setup. |
| WAL fixture green | Unchanged concurrent-reader regression and full launcher suite15/15 pass, exit0, `gates/harness-pib-green.log`. Native binaries are unchanged. |
| T019 dependency red/green | `gates/nac-consumer-revocation-red2.log`5 failed assertions versus `gates/nac-consumer-revocation-green.log`23/23; `gates/nac-revocation-red.log`2 failed versus `gates/nac-algo-error-green.log`4/4. Initial fixture startup failure in `nac-consumer-revocation-red.log` is not counted as behavioral reproduction. |
| T019 GDB/NDN_LOG | `gates/nac-late-content-gdb-red.log`, old test/library: catch `oabe::_OpenABE_ERROR`; main thread in SegmentFetcher/Consumer onCkeyData, crypto worker in ABESupport decrypt/importUserKey/constructKeyFromBytes. `nac-consumer-fixture-debug.log` identified AA registration ordering in the test setup. |
| T019 expanded dependency gates | Algorithm9/9,25 assertions (`gates/nac-algorithm-expanded-green.log`); real CP/KP integration3/3,42 (`gates/nac-integrated-expanded-green.log`), plus Consumer2/2,23:14 cases/90 assertions. Performance benchmark cases were not part of this correctness gate. |
| T019 NDNSF unit gate | Installed repaired NAC dependency:182/182 cases,11971 assertions, exit0, `gates/unit-nac-final.log`. |
| T019 NDNSF integration gate | Installed repaired NAC dependency:72/72 cases,1278 assertions, exit0, `gates/integration-nac-final.log`. |

NAC debug in `grant-crypto-diagnostic/user-B.log` establishes OA-9/OA-10:
initial DKEY fetch at startup, explicit renewal around12s coalescing behind it,
stale completion around15s rejected, then replacement installed. First ACK
decrypt attempts report no private key. The new component test separately
reproduces admission and callback ownership failures.

## MiniNDN history and final acceptance

| Retained attempt | Result | Interpretation |
|---|---|---|
| `baseline-grant` | Corrected target14/16, two timeouts | Old collector incorrectly reported14/14. This is a failed gate. |
| `late-grant-first` | Failed | Original timing missed exhaustion; exporter omission repaired. Later evidence identified constructor blocking, superseding the initial RSA-delay hypothesis. |
| `campaign` | 14/16 | Before asynchronous startup: late constructor blocking and inflight drain failed. |
| `campaign-final` | 13/16, driver exit1 | Before final readiness repair: missing normal renewal, missing late exhaustion, and offline resume timing failed. Other13 scenarios passed on that native candidate. |
| `campaign-renewal` | 2/3, driver exit1 | Late grant passed21/21 target and60/60 control;39 pre-renewal denials, one target DKEY refresh and zero unaffected refreshes. Offline role installed epochs1 then3. Normal grant failed12/13 target and12/24 control. |
| `campaign-grant-final` | Failed | With1s convergence, target11/13 and control23/24; this exposed initial-DKEY admission and callback loss. |
| `grant-crypto-diagnostic` | Failed, exit4 | Additional NAC Consumer diagnostics; no negative result was promoted. |
| `campaign-readiness-final` | Incomplete failed campaign:8/9 completed probes passed; driver intentionally stopped, exit143 | Normal grant10/10 target and24/24 control,11 denials, one target/zero unaffected DKEY refreshes. Late target21/21 but User/A startup exit1/control0/0 exposed shared-PIB contention. Active ninth probe completed normally after driver stop; no16-scenario acceptance is claimed. |
| `campaign-pib-final` | Incomplete failed campaign:5/6 completed probes passed; driver intentionally stopped, exit143 | Both grants pass (normal10/10 and24/24 control; late21/21 and60/60 control). Retry passes14 business checks but Provider/A aborts(-6), so the scene and campaign fail. Active scene cleaned up normally. |
| `campaign-nac-final` | **16/16, driver exit0** | Complete campaign using NAC `8b462d0`, WAL fixture and all-row/process-exit gates. Every manifest and29 current artifact hashes agree. |

Earlier T017 live closure in `campaign/revocation-rotation-failure-retry`
passed14/14 checks: epoch2 rotation failure, same-target recovery to epoch3,
all four roles installed3, revoked User had16 pre-withdrawal successes and no
post-failure successes, with denials both during and after recovery. Unaffected
User had16/16 post-recovery successes. The final campaign below repeats this
behavior after the User/Provider and NAC fixes. Every prior failed/incomplete
campaign retains its original status.

### Final complete campaign

Root: `campaign-nac-final/`; each result is
`campaign-nac-final/<scenario>/result.json`, with its adjacent `manifest.json`.
All16 results are completed, networkEvidence=true, gatePassed=true, launcher
exit0. Scenario durations total1033s. Checks below are the scenario evaluator's
checks; the two grants use the separate grantOnlyGateOk=true gate.

| Scenario | Checks / grant gate | Seconds |
|---|---|---:|
| `revocation-rotation-failure-retry` | 14/14 | 103 |
| `grant-after-permission-exhaustion` | grant gate PASS | 126 |
| `grant-only-advance` | grant gate PASS | 67 |
| `inflight-revocation` | 12/12 | 54 |
| `user-identity-revocation` | 12/12 | 54 |
| `provider-identity-revocation` | 14/14 | 63 |
| `service-scoped-revocation-with-unaffected-control` | 11/11 | 55 |
| `offline-rejoin-epoch-skip` | 8/8 | 65 |
| `controller-cache-provider-status-retrieval` | 14/14 | 55 |
| `controller-unavailable-expiry` | 7/7 | 59 |
| `large-response-invalidation` | 14/14 | 54 |
| `targeted-refill-invalidation` | 14/14 | 55 |
| `stream-invalidation` | 6/6 | 54 |
| `hintless-scheduled-refresh` | 6/6 | 55 |
| `controller-restart` | 15/15 | 58 |
| `selection-response-tamper-and-replay` | 14/14 | 56 |

Normal grant: target10/10, control24/24,11 pre-resolution denials, zero failed
target/control rows. Late grant: target21/21, control60/60,39 denials, zero failed
rows. Each has one target DKEY refresh and zero unaffected DKEY refreshes.
Late event timestamps in microseconds establish the ordering directly:
permission exhaustion1788644447075392 < grant1788644475366235 <
explicit App renewal1788644486073626 < first enqueue1788644487074818.

Rekey-failure recovery: failure epoch2, same-target retry epoch3, all four
roles installed3; revoked User had17 pre-withdrawal successes, zero successes
after failure, and denial during/after recovery. Unaffected User had14/14
post-recovery success. All five role processes exited0, closing the previous
Provider abort. Offline User installed epochs[1,3] and succeeded6 times after
rejoin; the revoked Provider made zero serving publications after discovery.

`gates/campaign-integrity.log` verifies the complete unique scenario set,
all gates/exits, manifest consistency and29 artifact hashes against current
disk. Campaign source is `7e5ef3676cc96be88e8c458a81e359e5d43fdae5`, with empty
working diff (`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`).
The final documentation checkpoint changes no tested runtime/launcher source.

## Build, reproducibility and operational boundary

The actual compiler is **GCC9** (`/usr/bin/g++ -B/usr/bin`), despite the output
directory name `build-clang-spec179-rv32`. Builds retain **-j2**. Native tests
embed framework source; App executables resolve the shared candidate framework.
Never rebuild shared libraries while MiniNDN processes are using them.

The installed patched NAC-ABE dependency is
`/tmp/nac-abe-spec179-exact-prefix/lib/libnac-abe.so`. Its existence and actual
`ldd` resolution passed again after the final rebuild. All five executable
targets and the shared framework have resolved dependencies with none missing;
Final SHA256 and `readelf` output are retained in `gates/native-nac-closure.log`;
`native-base-closure.log` retains the previous dependency snapshot.
The App RUNPATH includes that prefix and `$ORIGIN/..`. A reproducible deployment
must retain the exact patched dependency; the temporary prefix is not an
upstream-distribution claim.

Each new manifest records source revision, working-diff SHA256, executable and
resolved dependency hashes, role executable names, policy and scenario
configuration. The commands field lists roles rather than full argv; the
launcher and reproducibility commands below define the actual invocation.
T019 updates the local NAC-ABE `Experimental` dependency from `b1c9c4f` to
`8b462d09073b97bec1a7e145cefc898df0682c67` (not pushed). The reviewable delta is
`nac-abe-late-callback-fence-20260905.patch`. The installed library SHA256 is
`d0ee723a588b6af5c970437bfc8c74e7524420636bb1ee8774fc149d424f1ede`;
`gates/native-nac-closure.log` confirms all NDNSF executables resolve it without
missing dependencies. Public headers and ABI are unchanged; the framework/App
binaries are retained while their dynamic dependency is rebuilt and revalidated.
The first readiness build completed (`gates/build-readiness-green.log`,21m58s).
The base-entry repair then rebuilt all five targets successfully with `-j2` in
12m58.644s (`gates/build-base-admission-green.log`). T019 separately rebuilt the
NAC dependency with Clang10 and `-j2`; all expanded gates then passed.

```bash
./waf build --targets=unit-tests,integration-tests,App_User,App_Provider,App_ServiceController -j2
build-clang-spec179-rv32/unit-tests --run_test=RequestScopedConfidentiality,ControllerRevocationPolicy,ControllerRevocationState,GenericDynamicApi,RuntimeStatusStorePersistence --report_level=detailed
build-clang-spec179-rv32/integration-tests --run_test=ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,RequestScopedResponseConfidentiality,Spec175InvocationStream --report_level=detailed
NDNSF_CAMPAIGN_OUTPUT="$PWD/results/spec179-online-auth-20260905/campaign-nac-final" NDNSF_BUILD_DIR=build-clang-spec179-rv32 bash scripts/spec179_minindn_campaign.sh
```

Run unit, integration and MiniNDN gates sequentially after compilation. Initial
permission recovery stays App-owned; no runtime permission-polling thread,
new wire mode or remote key-erasure mechanism is added. Old disclosed keys can
still decrypt their historical ciphertext. Global ABE rekey after withdrawal
remains the current design. T014 upstream NAC-ABE publication is outside this
local goal and remains a separate maintainer action; no push is authorized.

## Workflow and audit dimensions

- Context Mode stats were an anomaly screen only. Project health passed; stale
  active hashes were repaired by canonical document indexing and active health
  passed. Repository tasks, source and artifacts remain authority. Final
  authority indexing and project/active health diagnostics are retained in
  `gates/context-final.log`.
- CodeGraph verified Controller recovery, real constructors and admission/
  hybrid decrypt callers. Final sync/status is retained in
  `gates/codegraph-final.log`. The sibling NAC repository has no CodeGraph
  index; its exact source, diff, real tests and GDB were inspected directly.
- Spec Kit constitution, feature artifacts, contracts, architecture and failure
  log were read. The bounded repair plan passed audit review; strict structural
  checks are retained in `gates/spec-audit-final.log`. RV-I35–RV-I39 map the
  completed component and final network evidence.
- GSD health passed; resumable local state is
  `.planning/debug/online-grant-revoke.md`; final health is in
  `gates/gsd-health-final.log`. Diagnosis is inline.
- ARS is not applicable: this is implementation/security regression, without
  literature, comparative-performance or statistical claims.

Intent/necessity: fixes follow reproduced failures. Ownership: Controller owns
authorization mutation, runtime owns readiness/enforcement, App owns renewal,
and launcher owns fault timing/evidence. Migration: no wire/store format change.
Rollback must never roll durable Controller epochs backward; reverting recovery
or readiness fixes reintroduces demonstrated failures. Evidence: component
crypto/state checks complement real wired MiniNDN and do not replace it.
