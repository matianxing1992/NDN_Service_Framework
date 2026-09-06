# Spec175 local closure archive

**Status:** historical evidence; not the current candidate authority.
**Current authority:** `../spec.md`, `../audit.md`, `../tasks.md`, and
`../handoff-to-spec180.md`.

**2026-09-02 scope correction:** Spec175 now ends at current-source local
unit/integration and CPU/MiniNDN qualification. Exact-SIF, CUDA, Qwen real-model
Tiger, and YOLO cross-model qualification moved to Spec180. Statements below
about an open SIF or Tiger gate describe the historical plan and no longer
authorize those actions from Spec175.

The active route no longer requires the historical G3/G4 matrix or a 42-entry
replay. As of 2026-09-02, focused tests (43), the C++ unit suite (605), and the
current host/CPU M01 production-path smoke pass. The current exact-SIF smoke
and the one Tiger deployment remain open; no older SIF or replay closes them.

Everything below is retained only to explain earlier failures and design
decisions. Do not use it to authorize a new job or combine it with current
candidate evidence.

## Archived checkpoint (2026-09-01)

The repaired pre-profile source passed G0, G1, G2, and the strict host/CPU G3
matrix. The durable diagnostic T020/T022 record is
[`t020-t022-canonical-transport-20260901.md`](t020-t022-canonical-transport-20260901.md).
It produced SIF
`sha256:b0f6a502e6646d2ad9054c95e29a50f2d1abd9f6b2e860fb2c93fbb00eadd992`,
which passed build/runtime preflights and an exact-SIF M01 smoke. The full G4
run was stopped after 24/42 results when the audit found that T024's claimed
historical-success delta checker did not exist at the production boundary:
the submitter/checklist bound files and PASS labels but did not compare a fully
rendered command/configuration with one proven D0/r23 profile or reject ambient
non-allowlisted values. The stopped run is diagnostic only and has no aggregate
PASS.

T024 now owns the checked-in proven profile, gate allowlists, semantic delta
validator, explicit sealed environment, and exact validated/submitted command
identity. Its tracked-source correction passes the focused mutation tests. The
post-correction source seal now has G0/G1/G2 PASS and the strict G3 manifest
`results/spec175/g3/qualification-manifest-post-profile-20260901T221454Z.json`
records 42/42 independent M01--M14 processes with four Providers and disabled
admission control. T020 and T022 are closed; T023 is the next gate.

### Post-contract exact-SIF candidate (2026-09-02)

The candidate built from the current source seal passed both the in-SIF runtime
and root host-substrate preflights. A bounded exact-SIF M01 smoke then passed
four-Provider Request/ACK/Selection/Response, emitted 293 structured lifecycle
events and terminal evidence with no surviving owned processes. Its bounded
forced teardown is recorded as intentional cleanup, not as a graceful-shutdown
claim. The smoke is retained in
[`t023-g4-smoke-post-contract-20260902.md`](t023-g4-smoke-post-contract-20260902.md).
T023 still requires one fresh three-repetition M01--M14 replay bound to this
candidate before any Tiger submission.

### Post-correction G0--G3 evidence

The seal SHA-256 is
`sha256:52c32e29a953b8f9f637a84853b49610d7059ef47c2654fef01fe4dc88bda111`.
The current G0 manifest (rerun after this closure document update) is
`results/spec175/g0/g0-post-correction-docs-20260901T221454Z.json`, SHA-256
`sha256:1d0f40630cf56e83e25d820679cc8b1f138b61876337da029690833452ca5671`.
The earlier same-seal G0 path remains preserved. The G1, G2, and G3 artifact
hashes are respectively
`sha256:0205f6d7a7fc0f16168c6a2331f3308d19005f90bfe65ba7957ef5e96215ea14`,
`sha256:e5e648d138a533b3f36330160b1e82904187d78ddc64b6689ab1bd7ee8558ea7`,
and
`sha256:c50e95eab7ac517a11e8afdf9e003d36381acd017b538b1df5dcdf7e8eaa217a`.
The first incorrect G2 invocation used `build/unit-tests`; the corrected gate
probed and ran `build/integration-tests` and passed 38/38 registered cases.
M13/r2 exposed two setup-only incidents (repository route probe timeout and a
Mininet host-configuration hang). They were preserved in their own output
roots, excluded from the aggregate, and followed by one same-tuple retry after
scoped Mininet cleanup. The final manifest includes only three PASS repetitions
for M13 and is the sole G3 authority for T023.

## Historical v5 checkpoint (2026-09-01)

The v5 SIF passed the local build-record and in-SIF runtime preflights. Its
host-orchestrated G4 replay is **BLOCKED**: M01 completed startup, route
readiness, four ACKs, Selection, and ordinary Responses, then failed when the
Providers fetched the canonical `/ndnsf-di/.../OBJECT/...` selected by V3.
They reported `nacabe.Consumer Data fetch error: Nack Error`; an explicit
`NDNSF_CONFIG` diagnostic reproduced the same failure. The tiny bootstrap
publishes repository transport names but does not publish the selected
canonical object name, so the old service route probe is insufficient. T023
remains open and no Tiger submission is authorized. Full details:
[`t023-g4-failure-20260901-canonical-object-nack.md`](t023-g4-failure-20260901-canonical-object-nack.md).

## Historical candidate (2026-08-30, superseded)

The newer candidate SIF `sha256:9d45d582bfe5f451884dd137c62b9bbd969293856d25ff48fbd107f715d6db6b`
passes both local SIF-runtime and root host-substrate preflights.  Its
fixed-seed exact replay is
`results/spec175/g4/exact-current-20260830f-fixed-seeds/replay.json` and is
**FAIL 41/42**: `M12-r2` timed out in the repository route probe before any
pipeline request, while an independent same-input M12 rerun passed.  The
 earlier one-seed replay and the isolated rerun are diagnostic only; neither is
 spliced into the aggregate.  G4 remains blocked until a fresh complete
 42-entry replay passes after the route-readiness owner is classified or fixed.

This candidate is now superseded as a release subject by the 2026-08-30
Provider-readiness correction.  The prior pipeline emitted READY before native
handler/filter registration; that source change invalidates this SIF and all
G0--G4 evidence above for promotion, even though the old SIF remains useful as
historical diagnostic evidence.

## Historical decision (superseded 2026-08-30)

The current source passes G0, G1, G2, and G3. One locally built SIF then
passed the separate exact-SIF runtime and host-substrate preflights. The
hash-bound exact-SIF replay
`results/spec175/g4/exact-sif-15f823a4/replay-manifest.json` contains 42/42
PASS entries: M01--M14, three independent processes per case, with the
registered M05--M09 negative or fault-injection oracles retained as PASS
results rather than treated as successful service responses. M11--M14 use the
real Provider conversation path.

The current G4 aggregate is a fresh replay with no replacement entry and no
overlay. The earlier replay42c aggregate and its explicit M14 replacement are
historical diagnostic evidence only; they are not mixed into this candidate.

The bounded M01 exact-SIF smoke also passed with four Providers and eight
token events followed by a final Response. Its reported `distributed_ms`
(8.0--8.9 s in the recorded runs) is an end-to-end tiny-ONNX CPU smoke metric,
not a GPU prefill/decode benchmark; the run does not contain separate prefill
or decode timing fields and cannot establish 27B CUDA performance.

## Historical status after readiness correction

The focused correction adds idempotent `ServiceProvider.start()` registration
and native start before the pipeline emits READY.  Source inspection confirms
that native `ServiceProvider::init()` runs synchronously inside `start()`;
Python and ndn-cxx logs use different streams, so a READY line appearing first
in a merged log is not evidence of an early marker.  The diagnostic
`results/spec175/g3/diagnostic-M14-provider-start-20260830k` passed M14 with
four Providers and a final Response.  A fresh M04 diagnostic,
`results/spec175/g3/diagnostic-M04-provider-start-20260830m`, likewise passed
with `providerCount=4`, `ackCount=4`, one wire Request, and a final Response.
These diagnostics do not replace any failed or incomplete repetition.  Because
the source changed after the recorded seals, the next release sequence is a
new source seal, G0--G2, a complete fixed-seed 42-process G3 matrix, one new
local SIF, and a new exact-SIF replay.  No Tiger submission is authorized by
these diagnostics.

### M01 route-readiness diagnostic (2026-08-30)

The first current-source M01 attempt under
`results/spec175/g3/current-20260830n/M01-r1` reached the user request with
only three ACKs; Stage 1 emitted no request/ACK evidence and V3 correctly
failed closed because no distinct feasible Provider was available for
`/LLM/Pipeline/Stage/1#0`. No case-result was written, so this attempt is a
retained failure and is not counted as a repetition. The bounded fresh retry
`results/spec175/g3/diagnostic-M01-retry-20260830o` used the same frozen seed,
topology, and workload and passed with four ACKs, one wire Request, eight
ordered token events, a terminal Response, and no surviving owned processes.
The pair initially classified an intermittent repository/SVS route-convergence
symptom; it did not authorize splicing the retry into a matrix. The subsequent
route snapshot in
`results/spec175/g3/diagnostic-M01-route-svs-20260830r/spec175-nfd-route-snapshot.json`
proved that every MiniNDN node had a route/FIB entry and multicast strategy for
both `/example/llm-pipeline` and the SVS group
`/example/llm-pipeline/group`. Therefore the failure was not an omitted NFD
route. The initial correction placed a five-second wait after all Provider
READY markers but before the User process existed. Three M01 diagnostics passed,
but this was not a valid User-to-Provider Sync barrier.

The pre-User interval passed M01 three times under
`results/spec175/g3/m01-svs-stability-20260830s/` each obtained four ACKs,
generated the exact eight-token sequence `[4,5,6,7,8,9,10,2]`, returned the
terminal Response, passed terminal cleanup with zero survivors, and completed
in 8036.45, 8121.55, and 8054.95 ms. The later formal-source attempt
`results/spec175/g3/current-20260830t/M11-r3` nevertheless failed its first
conversation request with only three ACKs; Stage 3 received no request. Its NFD
snapshot again passed on every node. The true missing boundary was that the User
joins the SVS group only when `APPClient/ServiceUser` is constructed.

The corrected launcher therefore records the full fail-closed NFD snapshot,
constructs the User, and makes the User itself wait a fixed five seconds before
publishing any Request. The case result records this as
`initialSyncSettleSeconds`; the User log records
`NDNSF_DI_USER_SVS_SETTLED` before `REQUEST_SENT`. The interval remains outside
measurement and does not change ACK timeout, request deadline, periodic Sync,
or workload. The focused contract suite passes 55/55. Three fresh M11 processes
under `results/spec175/g3/m11-post-user-sync-stability-20260830u/` each completed
two real network requests, received 4/4 ACKs in both windows, used two fresh
Request and Generation IDs, retained eight conversation entries, and passed
terminal cleanup with zero survivors. This closes the focused diagnostic only.
The source change invalidates the prior seal and all G0--G3 results; the complete
fixed-seed G3 matrix must run from a new root and retain every failed trace.

The next fresh matrix `current-20260830v` passed M01--M09 three times and M10-r1,
then M10-r2 failed before mutation in the repository STATUS probe. Its NFD
snapshot was PASS. More decisively, all three unique probe Requests reached and
validated at the Repo Provider within the attempt windows, but the newly
constructed publisher `ServiceUser` received no ACK. The repository publisher
therefore had the same post-join cold-start boundary as the main pipeline User.
`spec175_repo_bootstrap.py` now waits the fixed five-second interval immediately
after constructing that `ServiceUser`, records
`NDNSF_DI_REPO_USER_SVS_SETTLED`, and only then sends the first STATUS probe.
The focused tests pass 62/62. Three fresh M10 processes under
`results/spec175/g3/m10-post-repo-user-sync-stability-20260830w/` each passed
the route probe, real four-stage repository publish/fetch path, streamed
generation, terminal cleanup, and zero-survivor check. This is focused
diagnostic closure only: the source change again invalidates the seal and all
downstream candidate evidence.

### SVS group-fanout root cause (2026-08-31)

The prefix-only NFD snapshot above was insufficient. A later low-volume
lifecycle campaign reproduced a repository probe failure in which all three
Requests reached `SVS_PUBLISH_DONE`, while the Repo emitted no
`REQUEST_RECEIVED`, ACK-handler, or ACK publication event. Exact FIB comparison
found `/example/llm-pipeline/group` on router `a` in both runs, but the failed
run omitted the Repo face (`281`) from that prefix's next-hop set. The earlier
PASS therefore proved prefix presence, not complete multicast fanout.

The host launcher now makes only the User, four Providers, and Repo SVS group
origins; installs explicit idempotent router-to-member and member-to-router
group routes; and records/validates the exact expected face set. The diagnostic
root `results/spec175/g3/m01-route-fanout-20260831/` contains five independent
M01 processes. All five pass with six members, `missingNextHops={}`, matching
ACK-handler start/done/finish/published counts, a final Response, and clean
terminal evidence. Routine logs use the bounded ndn-cxx filter
`*=WARN:ndn_service_framework.TimelineTrace=WARN:ndnsf.di.RuntimeEvidence=WARN`;
global TRACE remains diagnostic-only. Because this changes the host runner and
G3 manifest contract, all older source seals and promotion evidence remain
historical; a fresh G0--G3 chain is required.

The first formal resealed M01 under
`results/spec175/g3/route-fanout-20260831b/M01/r1` completed the full business
flow but was rejected because an ndn-cxx WARN record split a multi-insertion
`std::cout` timing record inside `stage3-provider.log`. This was an evidence
serialization defect, not a Request/ACK/Selection/Response failure. Native
Provider and ONNX timing records now assemble one complete line and submit it
through the `ndnsf.di.RuntimeEvidence` ndn-cxx logger. The focused host-gate
suite passes 60/60, the focused production integration case passes, and the
final bounded-filter diagnostic
`results/spec175/g3/m01-runtime-evidence-20260831/M01-r4` passes with four
closed Provider spans, zero open spans, six verified SVS members, 4/4 ACKs,
the eight-token oracle, a terminal Response, and no malformed timing marker.
The rejected formal root remains negative evidence. Because native runtime and
launcher sources changed, a new source seal and complete G0--G3 chain are
still required; this diagnostic is not spliced into a formal aggregate.

## Historical hash-bound evidence

| Item | Path | SHA-256 |
|---|---|---|
| Source seal | `results/spec175/g0/source-seal-15f823a4.json` | `sha256:aedf3cf8faa870d07e41bf5a1865928f579305fb924430ee58781a3e16abb7a6` |
| G0 qualification | `results/spec175/g0/qualification-manifest-15f823a4.json` | `sha256:ad3f1dedccd7a09adddd389dbae16b6b9c8b117f1a914afead5cd9f479608201` |
| G3 host matrix | `results/spec175/g3/host-minindn-manifest-fe285147.json` | `sha256:62caf8067d7049093a1894dd520774443b5cc0d76444aef526ee2957f88b659f` |
| SIF runtime preflight | `results/spec175/g4/sif-runtime-preflight-15f823a4.json` | `sha256:b3c6f659b46a26c6d1bc53b0405c842e58c9b9a604d57ee4cad7de018b8f2cc9` |
| Host-substrate preflight | `results/spec175/g4/host-substrate-preflight-15f823a4.json` | `sha256:7071aba20316fab603c0f562f7d58adc987a10363a8636ad6b04f08641d3ac1e` |
| Exact-SIF replay | `results/spec175/g4/exact-sif-15f823a4/replay-manifest.json` | `sha256:4845c502cbf1fbac2dc91cb44d3468110b30e067817766fadcaac71dee9d7359` |

## Historical release boundary (superseded 2026-08-30)

T023/G4 and T025 are complete for the current candidate. T025 is recorded in
`evidence/t025-tiger-control-206901.md` and
`evidence/tiger-stage-readiness.md`: the current SIF passed the four-Provider
CPU control and the pinned three-stage Qwen3.6-27B CUDA state/cache-readiness
gate on Tiger jobs `206901` and `206907`. T026--T028 and T034 remain open: no
current record here claims multi-Provider generation, conversation residency,
or the 20-token/s performance target. The next valid step is the current-SIF
three-Provider functional gate, followed serially by conversation-residency
and performance gates.

## Historical diagnostic evidence

The 2026-08-28 replay42c records remain retained for audit and explain the
earlier M14 startup race, but their source revision and SIF hash differ from
the current candidate. They must not be cited as current qualification.
