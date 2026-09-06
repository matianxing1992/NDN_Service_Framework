# Spec 170 evidence identity

## Final audit

The current cross-axis audit is recorded in
[`final-audit-20260819.md`](final-audit-20260819.md). It is the entry point for
protocol qualification, negative-coverage status, performance evidence, and
remaining freeze/closure blockers; it does not replace the task ledger.
The durable PASS/BLOCK summary is in
[`closure-report.md`](closure-report.md), with the interim requirement index in
[`traceability.md`](traceability.md). Both remain BLOCK until T029/T036 and the
remaining lifecycle rows are actually closed.
The latest current-source host regression totals and exact binary hashes are
in [`current-local-regression-20260819.md`](current-local-regression-20260819.md).
The exact r23 SIF CLI/D0/D1 network rerun is recorded in
[`exact-sif-network-rerun-20260819.md`](exact-sif-network-rerun-20260819.md).
The latest current-worktree Python contract rerun is recorded in
[`spec170-python-contract-rerun-20260819.md`](spec170-python-contract-rerun-20260819.md).
The combined current-source C++/Python rerun is recorded in
[`current-local-regression-rerun-20260819.md`](current-local-regression-rerun-20260819.md).
The focused current-build post-Selection assignment/request/response rerun is
recorded in
[`spec170-postselection-integration-rerun-20260819.md`](spec170-postselection-integration-rerun-20260819.md).
The complete current-build native core-flow rerun is recorded in
[`spec170-core-flow-full-rerun-20260819.md`](spec170-core-flow-full-rerun-20260819.md).
The independent cross-Provider DATA_V1 unit rerun is recorded in
[`spec170-cross-provider-unit-rerun-20260819.md`](spec170-cross-provider-unit-rerun-20260819.md).
The matched switching-window re-analysis is recorded in
[`performance-opportunity-holdout-20260819.md`](performance-opportunity-holdout-20260819.md).
The auxiliary source-integrity failure is recorded in
[`current-python-negative-regression-20260819.md`](current-python-negative-regression-20260819.md).
The focused Gate A rerun (including the corrected quickstart test entry,
ndn-svs smoke, and NDNSF-DI core-flow integration) is recorded in
[`spec170-gate-a-focused-rerun-20260819.md`](spec170-gate-a-focused-rerun-20260819.md).
The performance-analysis contract/tool rerun is recorded in
[`spec170-performance-analyzer-rerun-20260819.md`](spec170-performance-analyzer-rerun-20260819.md);
it does not substitute for T036 measurements.
The combined performance claim-boundary rerun is recorded in
[`spec170-performance-claim-boundary-rerun-20260819.md`](spec170-performance-claim-boundary-rerun-20260819.md);
it confirms the guards remain active but does not close T036.
The Qwen3 model/runtime preflight and its pre-NDNSF Transformers mismatch are
recorded in
[`spec170-qwen3-toolchain-preflight-20260819.md`](spec170-qwen3-toolchain-preflight-20260819.md).
The corrected T014 protected dataflow/grant/capability/zeroization boundary and
its local negative-test qualification are recorded in
[`t014-protected-runtime-local-20260819.md`](t014-protected-runtime-local-20260819.md).

> **Current release route:** the local host builds and verifies one complete
> application SIF with Apptainer; TigerCluster only verifies its hash and
> executes it. Docker/Buildah/OCI archives, `docker://` pulls, and Tiger-side
> materialization are historical provenance, never current Spec170 inputs.

Gate evidence is bound to a single candidate and run by
`tools/ndnsf-di/spec170_evidence.py`.  The candidate digest covers source,
SIF, dependency lock, model, canonical artifacts, prompt corpus,
security policy, route, schedule, and freeze timestamp.  Complete and negative
rows are retained separately.  After `freeze()` any mutation is rejected as
`INVALID_CANDIDATE`; no TigerCluster result may be merged into a different
candidate identity.

An OCI digest may appear only in historical provenance for older candidates;
it is not a required input to the current local-SIF candidate.

The historical 2026-08-16 candidate identities are recorded in
`current-spec-audit-20260816.md`, `spec170-full-build-runs-20260816.md`, and
`gate-c-sif.md`; the sealed source sidecars remain in the protected source
staging directory and the exact SIF identity is recorded in the Tiger release
`SHA256SUMS`.  The former `.codex-tmp/spec170-candidate-worktree` copy was
removed as an obsolete duplicate during storage cleanup, so it is not an
authoritative evidence path.
The Python contract suite and local MiniNDN gate do not substitute for the
native TigerCluster D2b/D2h workload; those gates remain explicitly blocked
until the native cross-Provider path exists.

The current pre-dispatch mapping from Tiger cases to local integrated tests is
recorded in `in-process-tiger-case-matrix-20260817.md`. All listed local cases
pass; this is a prerequisite matrix, not a Tiger hardware result.

The real host MiniNDN cancellation/filtering loop is recorded in
[`real-minindn-cancellation-filterfix-20260819.md`](real-minindn-cancellation-filterfix-20260819.md).
It distinguishes the original stale-SVS-segment issue, the injected-delay
no-progress configuration failure, and the final passing Spec111 safety gate.

The source-preparation digest regression is recorded in
[`source-seal-determinism-20260819.md`](source-seal-determinism-20260819.md).
It proves that relocated source-only preparations receive the same
content-based seal while preserving validation of legacy path-bound seals; it
is not a T029 freeze record.

The corresponding coverage boundary is audited in
[`source-seal-coverage-audit-20260819.md`](source-seal-coverage-audit-20260819.md):
runtime source sealing passed, but build, harness, test, and configuration
inputs still require an explicit T029 manifest classification.
The first executable pre-freeze inventory of those broader inputs is
[`spec170-candidate-input-inventory-20260819.md`](spec170-candidate-input-inventory-20260819.md);
it is a review artifact, not a frozen-candidate record.

## SIF release pointer (updated 2026-08-19)

The concise operator handoff for the next update is
[`spec170-update-lessons-20260819.md`](spec170-update-lessons-20260819.md).
Read it before allocating storage or building; it records the current usable
SIF, fixed local/target configuration, one supported update route, and the
failure classifications learned from recent attempts.

There is currently **no T029-frozen Spec170 application SIF**. Candidate r13 was
retroactively revoked because it copied a host-built `_ndnsf.so` into the
image. Its successful import, `ldd`, closure, and focused runtime checks prove
behavior of those bytes but not container-native build provenance; they cannot
qualify the candidate. The r13 SIF, its job files, and all earlier r4-r12
artifacts are historical evidence only and must not be uploaded, executed, or
used as a base for a new candidate.

The last bounded-verification local candidate is r23. It was built with local
Apptainer 1.5.3 from sealed source revision
`989a9daace669a4f93496dade3176c527edb2469` and has SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`; its
source seal is `sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e`;
its
build record reports `containerNativeBuild=true`, `hostBinaryInputs=[]`, and
`tigerAction=verify-hash-and-execute-only`. The local path is
`.codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif`, paired with
`build-record-r23.json`. It is usable for that exact sealed revision and the
image probes recorded in
`current-sif-r23-local-20260819.md`, but it does not include uncommitted
working-tree changes and is not a substitute for the still-required T029
freeze and closure ledger. Any source/native/runtime mismatch requires a new
source identity and complete local SIF.

The next release pointer may be written here only after the candidate is built
through `build-local-sif.sh`, the pre-Apptainer build-boundary validator passes,
and the exact final SIF matches its `container-native-build.json` record.

The first runtime artifact in the normal process is a locally built application
SIF. The current Spec170 route uses the explicit local Apptainer binary
`/opt/apptainer/1.5.3/bin/apptainer` and a sealed localimage refresh. Docker,
Buildah, OCI archives, `docker://` pulls, and Tiger-side
materialization are outside this release contract. TigerCluster only verifies
the promoted SIF hash and executes that immutable file. Historical
GHCR/remote-builder, alternate-version probes, and OCI intermediates remain for
provenance only and are not candidate inputs.

The latest historically submitted Spec170 runtime was the locally built role-evidence GPU
SIF `spec170-runtime-edd1e096-role-evidence-python-20260817-r4`:

```text
/project/tma1/ndnsf-di/releases/spec170-runtime-edd1e096-role-evidence-python-20260817-r4/runtime.sif
sha256:585edf7805c1ffd57e72053fdbecf832da1730c70b0dc1def1caa86517ed9926
source: edd1e0965e688f996fb5f37ce80043ce67aa19ed
build: local Apptainer localimage refresh
local Apptainer: 1.3.4
Tiger Apptainer: 1.3.4-1.el9
```

The r4 SIF was copied to project storage and used by Tiger Job `197936`. Its
Python import, Provider hash, ONNX Runtime/CUDA readiness, and `ldd` resolution
passed, but the internal-library closure checker found forbidden build-host
RUNPATH entries in the Provider and `_ndnsf.so`. It is therefore
`BLOCKED_LOCAL_LIBRARY_CLOSURE`; do not reuse that release identity. The
complete negative result is in `spec170-sif-library-closure-r4-fail-20260817.md`.

For provenance only, the historical r5 candidate was:

```text
/project/tma1/ndnsf-di/releases/spec170-runtime-edd1e096-role-evidence-python-20260817-r5/runtime.sif
sha256:8db126c01bbdd4eb084b6ce5899c323a5cbca69ab223aca8009e4fae53254724
source: edd1e0965e688f996fb5f37ce80043ce67aa19ed
runtime RPATH: /opt/ndnsf-di/current/lib:/opt/onnxruntime/lib
```

The historical r5 local and staged checks both reported
`LOCAL_AND_STAGED_SIF_LIBRARY_CLOSURE_PASS`: Provider, Python extension, and
all 18 distinct packaged library payloads had resolved `ldd` closures, exact
lock hashes/SONAMEs, and no host/build RPATH. Those reports used the previous
lock format. The stricter current gate additionally requires
`ndnsf-sif-library-lock-v2` with a non-empty version on every library row and
same-root SONAME validation; r5 must be re-locked and rechecked before it is
eligible for a new submission. This is a packaging/runtime closure gate, not
yet a distributed-inference PASS. The historical staged report is
`staged-sif-library-closure-v3.json` (SHA-256
`b2b43987d9e3cfb248eaf9e9a4fa9991537450eb8d3b13b2cb34e17b9ab96a7a`) and was
run with validator SHA-256
`f63aa46ba0d8238ab23034d0ea96ce414dab79e31798bd2f253be669157cf692`.

## SIF/network workload contract (2026-08-16)

The SIF is an immutable runtime substrate; `--nv` only exposes CUDA libraries
and devices and does not change a manifest's execution provider. D0 uses the
current CPU manifest. D1 uses one source-aligned bundle derived from that same
bundle with an explicit CUDA manifest (`executionProvider=cuda`, `deviceId=0`,
`allowCpuFallback=false`). The plan, driver, policies, trust schema, and
artifacts remain identical. The bundle preflight permits only this registered
manifest difference, verifies all four role mappings, and requires the
Provider to run from the mounted bundle directory so relative artifact paths
resolve correctly. Failed historical D1 wrappers and stale bundles are not
reused.

This is also the human-usable operating rule: one immutable SIF, one
hash-verified read-only bundle, and one direct workload. The operator does not
assemble ad-hoc wrapper arguments or patch paths after allocation. `--nv`
exposes the allocated device; the bundle manifest selects CPU or CUDA, and the
preflight rejects a missing bundle `cwd`, missing relative artifact, stale hash,
or unregistered manifest difference before Slurm submission.

## Multi-role Selection wire invariant (2026-08-16)

The D1 wire trace established a separate protocol gate. A same-Provider
multi-role Selection carries a binary `OpaqueAssignmentSet` (one envelope per
role). The Selection projection, hybrid encryption, Provider decryption, and
local parser must preserve that TLV container; appending legacy semicolon
metadata such as `scopeKeyData.*` or `roleProvider.*` corrupts the payload and
causes the Provider to fall back to the service name as its role. That failure
must be classified as an NDNSF assignment transformation defect, not as a
SIF/CUDA or Tiger filesystem failure. The focused local regression is
`GenericOpaqueSelection/ServiceProviderPreservesStructuredAssignmentSetForCollaborationHandler`.

## D2 status after the D1 pass (2026-08-17)

D2a has two preserved r5 attempts. Job `198058` failed before dispatch because
the submission passed a host `/project/...` workload path instead of the
container `/release/...` path. Corrected Job `198064` staged the exact r5 SIF,
reached Provider readiness, authenticated ACK, and Selection, then timed out
before a Provider request/final Response; the Provider recorded no request or
handler event. The focused local regression
`PreconfiguredEnvironmentRunsSameProviderMultiRoleCollaboration` now covers
that missing path end-to-end and passes; the detailed diagnosis and command
evidence are in `post-selection-assignment-integration-20260817.md`. Thus the
remaining Tiger work is to regenerate the current bundle and rerun D2a, then
add focused local vertical cases for D2b cross-Provider dataflow and D2h
heterogeneous execution before submitting those cases.
