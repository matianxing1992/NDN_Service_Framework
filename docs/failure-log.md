# Failure Log and Evidence Index

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Spec181 formal Y-N R19 (2026-09-06): PASS; T008 preflight remains open.**
The maintained CLI completes all seven subcases, including three real grant
variants, on ce6a4ba0. All 63 application child exits are collected; source and
input identities remain unchanged, and no recorded PID or NFD remains.
See [R19 qualification](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-y-n-matrix-current.md#current-r19-qualification).
The next complete local gate needs case-specific configuration binding and
test-source closure; see [T008 preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md).

**Spec181 exact-wire native R1 (2026-09-06): PASS.**
Committed ce6a4ba0 passes maintained native build and independent verify.
Both focused regressions also pass against the refreshed Core (21 and 270
assertions). The affected 12-dimension convergence review restores A05 PASS;
proceed to a fresh R19 matrix, retaining all earlier failures. See
[native review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review).

**Spec181 exact-tensor R6 (2026-09-06): focused PASS.**
Nine real DI/Provider/IMS cases pass 270 assertions: 1.4 MB compact transfer
fits signed packets (maximum 8,477 bytes), legacy format reconstructs, and
signature/commitment/context/bounds plus authenticated inner HMAC/index/legacy
binding mutations reject at their named boundaries. Commit the shared repair,
refresh full native identity and re-audit before a new formal matrix. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6).

**Spec181 exact-tensor R4 (2026-09-06): legacy fixture exceeds packet limit.**
Large compact transfer and four real verifier rejections pass (5/6 cases).
The legacy fixture's 7,000-byte payload segment plus old metadata reaches
10,555 signed bytes; Core correctly rejects before decoding. Use small legacy
segments to test compatibility, retaining the unchanged 1.4 MB compact case.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4).

**Spec181 exact-tensor R2 (2026-09-06): test bridge correction required.**
Compact production transfer reconstructs the 1,400,017-byte object and all
signed packets fit; 412/413 assertions pass. The sole failure counts 404
packets versus 202 because both fixture peer bridges and manual bridges run.
Disconnect fixture peer bridges before custom forwarding; keep the same
packet-count, content and size assertions. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2).

**Spec181 exact-tensor R1 (2026-09-06): semantic RED at publication.**
The real DI publishOutput of a 1,400,017-byte tensor creates a 17,546-byte
signed manifest and correctly fails the repaired Core limit. Build passed;
the failure is the remaining codec boundary. Apply compact exact encoding
with authenticated reconstruction and legacy compatibility before retry.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1).

**Spec181 exact-wire Core R2 (2026-09-06): focused PASS.**
Initial inventory rendering separately rejected the new evidence file's missing
layer header; add the explicit scoped header before rerunning that document check.
The production Core rebuild and unchanged Provider/IMS regression pass all
21 assertions: 8,799/8,800-byte signed packets are readable; 8,801-byte packets
reject and a late batch size failure exposes no earlier item. DI compact
representation/consumer repair remains BLOCK before native refresh and matrix.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2).

**Spec181 exact-wire Core R1 (2026-09-06): semantic RED reproduced.**
The real Provider/IMS pull regression passes 18/21 assertions but accepts an
8,801-byte signed Data and leaves an earlier batch item readable after a late
oversize item. Adopt full signed-wire prevalidation for the whole batch, then
repeat the unchanged test in a fresh run. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1).

**Spec181 R18 diagnosis (2026-09-06): BLOCK at NDN Data wire size.**
The first actual send boundary is now identified: BackboneNeck's exact
MANIFEST Data encodes to 19,658 / 10,883 bytes and a SEG Data to 14,191,
above ndn-cxx's 8,800-byte limit (169 event-loop exceptions). The exact
NDNSF-DI filter exists; downstream deadlines are consequences. Review the
existing compact transport changes and pre-publication size guard as a
bounded shared unit, retaining signature/content commitments. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 T005 formal R18 (2026-09-06): BLOCK at exact dependency transfer.**
BackboneNeck now executes with actual CPU ONNX evidence and completes its
role. DetectShard0/1 cannot fetch its exact tensor manifests; Merge then
times out on their outputs. Inspect publication, cache response and routing
before another run. Source/input identities remain unchanged and NFDs exit.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 backend native refresh R1 (2026-09-06): PASS.**
Committed 214df1d6 completes maintained native build and independent verify;
both report native identity OK. The tested registration change and retained
device/error contracts pass affected convergence review. Resume a new formal
matrix; focused CPU checks do not establish matrix qualification. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1).

**Spec181 backend repair R4 (2026-09-06): focused PASS.**
The maintained native Provider rebuilds (35.522 s); five real executable
checks pass (1.02 s). Legacy/public CPU names load and warm a real ONNX
model; unknown names and invalid execution-provider metadata still reject.
Commit only registration blocks and tests, then refresh native identity and
re-audit before the next matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4).

**Spec181 backend probe R3 (2026-09-06): semantic RED reproduced.**
With corrected evidence assertions, legacy CPU load/warmup and unknown-backend
rejection pass; three public-name checks fail at missing registration. Adopt
only the two registration blocks, rebuild the maintained Provider and repeat
the real executable checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-regression-r3).

**Spec181 backend probe R2 (2026-09-06): registration RED and assertion correction.**
Three checks reach missing public backend registration. The legacy backend
actually loads/warms the model, but its test misreads existing string-valued
evidence and nested device fields. Unknown-backend rejection passes. Correct
the schema assertion before the next focused run. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r2).

**Spec181 backend probe R1 (2026-09-06): BLOCK at test collection.**
An extra parenthesis in the new test prevents collection (0.36 s); no native
process ran. Correct the test syntax and retain R1 before a fresh R2 probe.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r1).

**Spec181 T005 formal R17 (2026-09-06): BLOCK at backend registration.**
All four Providers pass external assignment validation and enter their handler.
BackboneNeck then fails with no NativeModelRunner backend registered:
onnxruntime-cpu; dependent-role fetch deadlines follow. Preserve that first
boundary, repair actual backend registration and re-audit before retry. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17).

**Spec181 digest native rebuild R1 (2026-09-06): PASS.**
Committed e6f44b65 rebuilds the Core library, native Provider and Python
extension; maintained build and independent verify both report native identity
OK. Source/byte checks and exact assignment rejection remain intact. Affected
convergence review PASS; proceed to a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1).

**Spec181 digest repair R3 (2026-09-06): focused PASS; native rebuild pending.**
Four actual C++ helper checks pass (1.73 s) after the isolated Provider emits
canonical lowercase hex. Exact assignment size/digest checks remain intact.
Commit this unit, rebuild the native runtime and re-audit before a new matrix.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3).

**Spec181 digest probe R2 (2026-09-06): RED reproduced.**
All four actual-helper comparisons fail only at Provider uppercase hex;
User matches independent SHA-256. Normalize Provider output to the existing
canonical lowercase contract, preserving exact byte/size checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-regression-r2).

**Spec181 digest probe R1 (2026-09-06): BLOCK at linker selection.**
The focused helper probe fails at compilation/linking (4 setup errors,
1.60 s), before digest comparison: system g++ selects Homebrew ld from PATH.
Use the system toolchain explicitly and retain the original log. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-probe-r1).

**Spec181 T005 diagnostic R16 (2026-09-06): BLOCK at external assignment validation.**
Core INFO logging shows each Provider receives and queues Selection, then
fails assignment preparation with external collaboration assignment size or
digest mismatch. The duplicate request log is not the controlling boundary.
Inspect assignment publication, fetch and validation on the same source.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#external-assignment-boundary-r16).

**Spec181 T005 formal R15 (2026-09-06): UNQUALIFIED after Selection commit.**
The sealed-plan repair reaches four-role Selection commit, then the User
receives REMOTE_RESPONSE_FAILED. Provider WARN logs contain duplicate request
rejections but no native execution failure reason; this does not yet identify
the controlling cause. Inspect the Core request/Selection boundary and obtain
bounded diagnostic logs before changing behavior. Source/input identities
stay unchanged and all NFDs exit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-response-boundary-r15).

**Spec181 sealed-plan repair R3 (2026-09-06): focused PASS.**
All 22 sealing/candidate checks (0.97 s) and 36 existing plan integration
checks (0.77 s) pass in the isolated source. Fetch references remain separate
from canonical identity, immutable and digest-bound; malformed values reject
before commit, while legacy/local-preparation defaults remain compatible.
Affected A05 review PASS; resume a new formal matrix after committing. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-repair-r3).

**Spec181 sealed-plan validation R2 (2026-09-06): BLOCK at reference validation.**
The projected field resolves sealing; five normal/legacy/coverage checks pass.
Five malformed reference checks fail because non-string false values and
control characters are accepted. Require strings and reject control chars,
retaining the deliberate empty local-preparation reference. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-validation-r2).

**Spec181 sealed-plan regression R1 (2026-09-06): RED reproduced.**
Ten focused checks reproduce the missing field at the actual V3 sealing
expression (1.72 s). Project the existing shared contract and validate
transport forwarding, digest binding, legacy defaults and invalid references.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-regression-r1).

**Spec181 T005 formal R14 (2026-09-06): BLOCK at sealed-plan reference closure.**
The shared assembly repair permits request planning to reach plan sealing.
The committed SealedCollaborationPlan lacks artifact_fetch_data_names,
already consumed by placement. Review/adopt the existing field and digest
binding, verify plan production/consumption, then re-audit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-boundary-r14).

**Spec181 canonical binding/assembly repair R6 (2026-09-06): focused PASS.**
The isolated Waf parity target builds successfully; all 40 canonical,
candidate and actual C++/Python assembly checks pass (12.91 s). Actual YOLO
two-candidate binding, recipe and publication-port checks pass as well.
Affected convergence review PASS. Commit only the tested shared CPU unit,
then resume a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-and-assembly-repair-r6).

**Spec181 canonical assembly R5 (2026-09-06): BLOCK at focused harness inputs.**
Thirty-two Python checks pass. Eight native checks lack the required parity
binary; the extended recipe probe incorrectly includes the non-ONNX Merge
role in its graph assertion. Correct those focused harness inputs before
further validation; no network attempt was made. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-assembly-boundary-r5).

**Spec181 canonical recipe R4 (2026-09-06): BLOCK after binding repair.**
R3 actual binding/publication and 24 focused checks pass. Extending the probe
through real role certification reveals that the committed recipe rejects
COMPONENT_SET zero intervals. Add the reviewed component/external-initializer
assembly changes to this source unit; retain unrelated CUDA provider selection
outside the unit. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-repair-r3-and-recipe-boundary-r4).

**Spec181 canonical-binding regression R2 (2026-09-06): RED reproduced.**
With the complete Python path and private offline NDN environment, actual
YOLO describe reproduces the missing canonical_graph_digest TypeError.
Proceed with the reviewed shared dependency unit and focused checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r2).

**Spec181 canonical-binding regression R1 (2026-09-06): BLOCK at probe import.**
The isolated probe lacks the Repo Python path and stops at SDK import before
the binding constructor. Complete the explicit Python/private NDN environment
for the next focused run. The source/reference contract,
shared deployment consumer and their tests form the reviewed dependency unit.
Validate that unit plus actual two-candidate publication before re-auditing.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r1).

**Spec181 T005 formal R13 (2026-09-06): BLOCK at canonical artifact binding closure.**
The epoch repair permits the User to request a V3 task. YOLO describe passes
canonical_graph_digest to the committed CanonicalArtifactBinding, whose
dataclass lacks that field. The working tree contains the shared canonical
reference extension, while the isolated commit omits it. Review and validate
the complete binding/publication dependency before another run; all NFDs
exited and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-boundary-r13).

**Spec181 subcase-epoch repair R2 (2026-09-06): focused PASS.**
All 117 runner/matrix/grant seam checks pass (3.99 s). Child epoch now matches
the publication/Provider runtime inputs; the protected Y-B and all three
Y-N-E mutations retain their grant configuration, and the parent environment
is unchanged. Affected convergence review PASS; T005 needs a new formal run.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-repair-r2).

**Spec181 subcase-epoch regression R1 (2026-09-06): RED reproduced.**
Seven plaintext cases inherit the protected matrix epoch; four protected
profiles pass. The focused production runner capture stops before network
startup (7 failed / 4 passed / 88 deselected, 2.06 s). Bind child epoch to the
same runtime inputs used by publication and Provider process specifications.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-regression-r1).

**Spec181 T005 formal R12 (2026-09-06): BLOCK at subcase epoch environment.**
Committed fixture loading passes. Y-N-O User inherits the matrix's protected
epoch, although runtime publication and process specifications choose the
plaintext control epoch; its grant seam then raises SPEC181_REQUESTER_PRIVATE_KEY
KeyError. Scope the child epoch to the selected subcase and regression-test
both plaintext controls and protected Y-N-E before rerunning. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-boundary-r12).

**Spec181 fixed-input closure repair (2026-09-06): focused PASS.**
The existing PPM and provenance README are adopted without byte changes.
All 22 numerical regression checks pass (7.97 s); the actual canonical
package reference loader verifies the fixture digest and shapes. A05 is
re-audited PASS; verify the committed fixture in the isolated checkout
before resuming the formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixture-closure-repair).

**Spec181 T005 formal R11 (2026-09-06): BLOCK at fixed fixture source closure.**
The repaired lock permits Controller publication, Repo, and four native
Providers to become ready. User startup fails because the isolated commit
lacks tests/fixtures/spec180/yolo26n/fixed-fixture.ppm. The existing untracked
162-byte fixture matches the manifest digest. Reopen A05, adopt the fixture
and its provenance, validate the real reference loader, then re-audit before
the next matrix. Source/input identities stayed unchanged; NFDs exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixed-input-source-closure-r11).

**Spec181 T005 formal R10 (2026-09-06): BLOCK at Controller publication initialization.**
The explicit shell environment repaired process spawning. NFD readiness and
Controller startup pass, but the co-located publication ServiceUser constructor
throws `Failed to acquire file lock`. The maintained matrix exits 2 at Y-N-O;
The focused syscall probe finds EACCES before flock: the UID-0 lock path is
owned by UID 1000, with no kernel lock or fuser occupant. Preserve this stale
file in R10 before letting the runtime recreate it; no Core change is needed.
no protocol result is established. Source/input identities remain unchanged,
and all NFD processes exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10).

**Spec181 T005 formal R9 (2026-09-06): BLOCK at explicit shell environment.**
The new diagnostic identifies KeyError in Mininet node.py:419: shell=True reads
os.environ['SHELL'], absent from the launch environment. Preserve the exact
frame chain; set SHELL=/bin/bash explicitly for R10. No Controller was launched,
all NFDs exited, and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r9).

**Spec181 spawn diagnostic repair R3 (2026-09-06): focused PASS.**
All 93 runner/matrix checks pass. Partial-start cleanup and first-failure stop
remain intact; the new exclusive diagnostic records type and frame locations
without exception text or locals. The actual R8 spawn cause still requires a
new run. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-repair-r3).

**Spec181 spawn diagnostic R2 (2026-09-06): BLOCK at test import.**
Two-file checks produce 1 failed / 92 passed (1.55 s). The runtime writes its
new diagnostic; the new assertion lacks the json import. Preserve R2, add the
test import, and rerun. This is a test-fixture failure, not a runtime result.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r2).

**Spec181 spawn diagnostic regression R1 (2026-09-06): RED reproduced.**
The existing partial-start cleanup test now verifies a durable error boundary;
it fails because process-start-failure.json is absent (1 failed / 87 deselected,
0.90 s). Keep cleanup and failure verdicts intact; record only exception type
and frame locations, preserving first evidence without messages or locals.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r1).

**Spec181 T005 formal R8 (2026-09-06): BLOCK at application spawn diagnostics.**
NFD readiness/routing/keychains pass. Controller log creation is followed by an
immediate spawn failure; the matrix preserves only CONTROL_NOT_PROVEN and drops
the underlying traceback boundary. Preserve R8 and add exception type plus
file/function/line frames, without exception text or locals, before another
diagnostic run. NFD cleanup was verified. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r8).

**Spec181 T005 formal R7 (2026-09-06): BLOCK at NFD readiness.**
All five NFD sockets exist, but node nfdc checks fail; no application child
started. The launcher inherited offline probe NDN_CLIENT_* overrides pointing
to unused.sock instead of node client.conf. Preserve startup diagnostics and
logs. R8 will isolate the parent via private HOME, remove the global overrides,
and retain explicit Python dependency paths. NFD/native Provider cleanup was
verified. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-network-boundary-r7).

**Spec181 T005 formal R6 (2026-09-06): BLOCK at Mininet executable readiness.**
The explicit PATH omitted sbin and Mininet could not find ifconfig (exit 1).
Preserve R6. Verify required network tools and append the system sbin paths for
R7 while preserving Python/native resolution order and recording the new launch
environment. This is startup readiness, not a protocol result. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r6).

**Spec181 T005 formal R5 (2026-09-06): BLOCK at launch environment parsing.**
The temporary parser rejected the registered SPEC180_CASE_OUTPUT_DIR in the
Y-N environment. No case/state or runner was created. Preserve R5; accept that
specific field and override it with R6's unique output. The five-role Y-N input
uses the same model; the protected epoch remains explicit for Y-N-E. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r5).

**Spec181 T005 formal R4 (2026-09-06): WAITING_EXTERNAL_INPUT at case roles.**
Envelope ownership now passes. The Y-B baseline configuration lacks Y-N's
required FullModel capability, so maintained validation exits 78 before network.
Preserve R4; inspect and use the existing Y-N-specific inputs for a new R5.
The registered role-set requirement remains unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r4).

**Spec181 T005 formal R3 (2026-09-06): WAITING_EXTERNAL_INPUT at key ownership.**
The repaired sudo source gate passes on 88e7a458. Maintained input validation
rejects the developer-owned envelope key for root execution (exit 78), before
network. Preserve R3 and provision the same bytes as a 0600 root-owned file in
R4's private state, retaining the original key untouched. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r3).

**Spec181 sudo source repair R2 (2026-09-06): PASS; formal R3 next.**
The gate preserves SUDO_UID only for root plus the actual selected checkout
owner. Real sudo positive/negative tests and existing local gate regressions
pass: 51 checks (8.68 s), with Git overrides still stripped. A05 is re-audited
PASS; T005 remains incomplete and will use a new run directory. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-repair-r2).

**Spec181 sudo source regression R1 (2026-09-06): RED reproduced.**
Real sudo Git checks yield 1 failed / 1 passed: the legitimate owner is rejected,
while the wrong UID remains rejected. The test also supplies hostile GIT_DIR
and GIT_INDEX_FILE overrides. Preserve red.log and repair only the matching
sudo-owner identity in the sanitized environment. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-regression-r1).

**Spec181 T005 formal R2 (2026-09-06): BLOCK at sanitized source Git.**
Outer Git now accepts the actual sudo user, but production _source_git drops
SUDO_UID again and fails before network. Reopen A05's sudo checkout boundary;
add a real sudo regression and retain the UID only when it matches the selected
checkout owner. All Git override variables remain stripped. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r2).

**Spec181 T005 formal R1 (2026-09-06): BLOCK at launcher Git ownership.**
The explicit environment dropped SUDO_UID; root Git rejects the user's checkout
before invoking the maintained runner. Preserve R1. Retain the actual sudo
caller UID in the explicit launch environment for R2; do not write global Git
exceptions or weaken the source gate. No network started. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r1).

**Spec181 T007 convergence (2026-09-06): PASS; T005 next.**
A05's source/configuration/input/build/application boundaries now map to their
focused regressions and final committed checks. A01–A12 are closed within their
documented scopes. The 12-principle audit permits same-source local validation;
it is not qualification or development-delivery PASS. Historical failures below
remain preserved. See [audit](../specs/181-ndnsf-di-protected-grant-qualification/audit.md#a05-closure-matrix).

**Spec181 committed source R13 (2026-09-06): R12 resolved.**
Repo build intermediates are preserved outside the checkout. The strict source
guard passes for 6b9bb51c, and the real application/native preflight plus four
application imports pass on that commit without network. Overall T007 audit
remains the next gate. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13).

**Spec181 committed source R12 (2026-09-06): BLOCK at build intermediates.**
The isolated checkout matches 6b9bb51c with a clean tracked/index tree. The
actual source gate rejects untracked Repo setup build/src/ArtifactManifest.o.
Preserve the terminal result and move the complete intermediate build directory
to R12 before retry, retaining runtime extension and strict source checks. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-reconciliation-r12).

**Spec181 source closure R11 (2026-09-06): focused failures resolved.**
The same isolated selected source passes 45 existing checks and 12 new candidate
binding checks. Actual application/native preflight and four application imports
pass without network. R1–R10 remain below as historical first-boundary evidence.
Final committed-source reconciliation and T007 audit remain open. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#focused-closure-r11).

**Spec181 focused source closure R10 (2026-09-06): BLOCK at request contract.**
Actual application/native preflight passes without network. Six-file checks
yield 9 failed / 36 passed (1.38 s): missing DIRequestEnvelopeV2 input transport
fields and InferenceApplication task arguments. Preserve R10 and close the two
request endpoints before retry. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-and-focused-r10).

**Spec181 application preflight R9 (2026-09-06): BLOCK at verifier implementation.**
Export alone is insufficient: ProviderOfferTrustVerifier itself is absent from
committed SDK provider.py. Preserve R9 and validate the implementation and its
existing signature/ACK tests as part of source closure. No network ran. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r9).

**Spec181 application preflight R8 (2026-09-06): BLOCK at public verifier export.**
Actual publication/process_specs/native guard pass. The post-guard import probe
then finds user.py requires ProviderOfferTrustVerifier missing from SDK exports.
Preserve R8 and add the existing verifier export; no network ran. Native guard
success alone does not prove application import closure. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r8).

**Spec181 application preflight R7 (2026-09-06): BLOCK at local launch helper.**
Runtime publication passes. Actual process_specs calls legacy python_cmd with
repo/py_dir, which the committed helper lacks. Close the matching local helper
parameterization, retaining default compatibility. Preserve R7; no network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r7).

**Spec181 application preflight R6 (2026-09-06): BLOCK at catalogue conversion.**
SDK/Provider imports pass. Runtime publication requires the uncommitted
PreSplitCatalogSnapshot.from_mapping contract. Preserve R6 and close the
matching validation/serialization dependency before retry; no native guard or
network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r6).

**Spec181 application preflight R5 (2026-09-06): BLOCK at Repo reference contract.**
The selected ApplicationInput contract requires LargeDataReference, absent from
committed repo_reference.py. Existing Provider/client/facades already consume
this shared publication/reference owner. Preserve R5 and close that dependency;
no network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r5).

**Spec181 application preflight R4 (2026-09-06): BLOCK at shared input contract.**
Candidate construction now passes. SDK imports then fail because the committed
Provider requires MAX_INLINE_INPUT_BYTES absent from adapters.base; the
coordinator also requires InputTransportMode. Preserve R4 and close the shared
input contract/export dependency before retry. No native guard/network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r4).

**Spec181 application preflight R3 (2026-09-06): BLOCK at shared candidate contract.**
Repo's same-source build and import pass. Production YOLO publication then
constructs SplitCandidate with selection_priority, absent from the committed
contract, and fails before native guard/network. Preserve R3; close the exact
shared contract dependency without mixing unrelated worktree changes. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r3).

**Spec181 application preflight R2 (2026-09-06): BLOCK at Repo Python extension.**
Adding four existing YOLO source files to the isolated checkout clears actual
catalogue verification. Policy generation then cannot import
py_repoclient._py_repoclient, which has not been built in that checkout.
No native guard/network started. Preserve R2; use Repo's maintained build
against the same source, not an unknown worktree binary. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r2).

**Spec181 application preflight R1 (2026-09-06): BLOCK at committed Python adapter import.**
The clean a51f87b3 native build passes and emits its new receipt. Actual runner
validate_inputs then fails to import build_yolo26n_adapter from adapters.yolo,
wrapped as CANONICAL_CATALOGUE_VERIFY_FAILED. No network or native guard was
started. Preserve R1 and repair the committed adapter package closure before
retry. See [local runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r1).

**Spec181 local input identity R3 (2026-09-06): BLOCK at new fixture import.**
After R2 66 PASS, new input tests yield 3 failed / 69 passed (7.68 s): three
tests reference json without importing it, before the identity owner runs.
Real-child drift/collection checks pass. Preserve R3, repair the fixture import,
then rerun. See [local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md#focused-r2-and-fixture-failure-r3).
R4 fixes the missing import: 72 PASS (8.40 s). Actual fixture children preserve
completed results and reject input drift before network cases or at final
aggregation. Both CLI discovery paths consume the explicit environment.
Configured input identity unit CLOSED; actual runtime audit remains T007 work.

**Spec181 local input identity R1 (2026-09-06): BLOCK at external input bytes.**
Model/map/referenced-key replacement and package additions reach the forbidden
child boundary after inventory creation (4 failed); launch configuration binds
path strings only. No qualification child ran. Preserve R1 and bind the actual
external inputs in the shared inventory/gate owner. See
[local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md).

**Spec181 explicit config root R1 (2026-09-06): BLOCK at protected launch selection.**
The runner overwrites explicit NDNSF_SPEC180_CONFIG_ROOT with the HOME default.
Absolute/relative overrides and missing-key rejection fail (3 failed / 1 passed,
0.81 s); fixture stops before native preflight/network. Preserve R1 and honor
the explicit root before child HOME changes. See
[explicit configuration root](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-explicit-config-root-20260906.md).
R2 honors the explicit root, resolves it before child HOME/cwd changes, and
rejects a missing explicit key even when the default key exists: 24 focused
checks PASS (0.85 s). Configuration selection unit CLOSED; T007 remains open.

**Spec181 Waf tool identity R3 (2026-09-06): BLOCK at CLI fixture PATH mismatch.**
1 failed / 79 passed (1.81 s): an old CLI linkage test builds with fixture PATH
then verifies with ambient PATH. The new Waf identity check correctly rejects
that mismatch first. Align the CLI fixture environment and retain its original
wrong-Core rejection assertion. R3 log and patch preserved in
[Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#cli-fixture-boundary-r3).
R4 repairs the CLI fixture: 80 PASS (1.75 s). Actual Waf directory selection
matches the new owner in both current and isolated checkouts (80 source/resource
files each). Waf source/selection unit CLOSED; runtime/input closure remains
T007 work. Existing native receipts require a maintained rebuild for the new field.

**Spec181 Waf tool identity R2 (2026-09-06): BLOCK at fixture executable identity.**
57 failed / 18 passed (2.77 s): the existing fake interpreter lacks its
executable bit, so real PATH resolution rejects it before mocked build; one
environment assertion also predates child-only WAFDIR. Preserve R2 and repair
fixtures before retry, without relaxing production resolution.
See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#fixture-boundary-r2).

**Spec181 Waf tool identity R1 (2026-09-06): BLOCK at generated build-tool identity.**
Four waflib content/location mutations escape native verification; interpreter
drift is rejected only after the native probe. Focused fixture checks: 5 failed,
70 deselected (0.36 s), no real build/network/qualification process.
Preserve R1, then bind actual selected Waf implementation in the maintained
native identity owner. See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md).

**Spec181 local configuration R1 (2026-09-06): BLOCK at launch identity.**
Six focused mutations of environment, reserved output variable, declared
digest and interpreter bytes reach the forbidden qualification-child boundary
(6 failed, 0.78 s). The runner already uses explicit environment; the missing
check binds its actual values to the declared digest. No qualification child
ran. Preserve R1 before repairing the shared launch-configuration owner.
See [local configuration identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-config-identity-20260906.md).
R2 binds actual launch inputs through one shared builder/gate owner (52 PASS).
R3 verifies real child environment consumption and post-execution identity
failure (61 PASS). R4 adds builder/gate CLI round-trip and rejects changed
configuration before output/children: 62 PASS (5.80 s). Launch configuration
unit CLOSED; T007 remains BLOCK at runtime/input-byte identity planes.

**Spec181 revision 7 (2026-09-06): BLOCK at local configuration identity.**
Committed-source validation is closed in 2628e3d2 (39 focused checks and
the configured checkout PASS). The remaining controlling work binds actual
local configuration, build/runtime dependencies and development delivery.
By owner decision, SIF/replay/Tiger move to the experiment machine and are
not local closure prerequisites. Git merge is deferred until development ends.
See [scope transfer](../specs/181-ndnsf-di-protected-grant-qualification/evidence/development-scope-transfer-20260906.md)
and [current tasks](../specs/181-ndnsf-di-protected-grant-qualification/tasks.md).

**Spec181 local gate identity R8 (2026-09-06): checkpoint hook rejection.**
The local commit hook rejects assistant-directory references in production
source validation. Remove those non-product exclusions and rerun the focused
checks; do not bypass the hook. No checkpoint was created by the failed commit.
R8 removes the exclusions: 39 focused checks PASS (4.46 s), and the actual
configured checkout passes SOURCE_CHECKOUT_OK. The hook remains enabled.
The retry checkpoint succeeds as 2628e3d2; this hook incident is closed.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8).

**Spec181 local gate identity R7 (2026-09-06): source unit CLOSED; configuration BLOCK.**
All 39 focused checks pass (3.44 s), and the configured 1ba99000 checkout
passes exact source validation. Bad preflight identity has no qualification
child/output side effects; mutation during fixture execution yields
UNQUALIFIED while preserving child/cleanup records. Effective configuration,
generated build-tool/runtime bytes and external import bindings remain open.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#focused-green-r7).

**Spec181 local gate identity R6 (2026-09-06): BLOCK at Python compatibility.**
New symbolic-link checks use Path.is_relative_to, absent from the maintained
interpreter. Focused checks yield 23 failures / 16 passes; the subsequent
read-only checkout probe hits the same AttributeError before qualification.
Preserve R6; use relative_to with ValueError handling, then require focused
success before the next checkout probe.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).

**Spec181 local gate identity R4 (2026-09-06): BLOCK at Git LFS representation.**
The configured 1ba99000 checkout is rejected because a committed 134-byte LFS
pointer represents a materialized 240376592-byte release archive. Preserve
R4 before adding exact pointer size/SHA-256 verification; do not classify this
as source tampering or a protocol failure. No qualification child ran.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R5 closes exact LFS-byte verification and passes 33 focused checks (2.51 s).
The next configured-checkout rejection is generated Waf tool code; classify
only its exact generated layout under the separate build-tool identity plane,
whose byte binding remains an A05 obligation. Preserve both R5 logs.

**Spec181 local gate identity R1 (2026-09-06): BLOCK at source authority.**
The real local gate accepts a fixture root without a Git HEAD and an invented
40-character sourceRevision, then reports PASS for six fixture children.
All six exits/cleanup records are collected; no network qualification ran.
Validate actual checkout/source identity before any qualification child or
output directory is created, then retain focused regressions for rejection.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R2 adds real Git fixtures and nine identity mutations; all nine reach the
forbidden child boundary instead of being rejected (9 failed, 0.63 s).
No qualification child runs; preserve the RED log and source patch before
adding checkout, index, tracked-byte and untracked-code checks.
R3 passes all nine rejections and existing gate/inventory checks (23 PASS,
2.39 s). Actual configured-checkout and submodule boundaries remain under
focused review; configuration/candidate identity is not yet closed.

**Spec181 native plan closure R1 (2026-09-06): BLOCK at projection behavior.**
Twelve missing header lines close the maintained local native build, including
the extension import/identity check. Focused plan/merge tests then yield
27 PASS / 2 FAIL: COMPONENT_SET postprocessing is rejected, and two PIPELINE
tensors collide in runtime scope with mismatched producer/consumer names.
Preserve R1 before applying the exact parser/scope repair; no qualification
matrix or model run was started.
R2 closes the parser/scope unit: 29 cases / 133 assertions PASS, including
unchanged transport authorization groups. Refresh native identity from its
source checkpoint before advancing the remaining A05 candidate/config audit.
The 1ba99000 checkpoint subsequently passes the maintained native build,
including a fresh extension import and runtime identity receipt (R3).
See [native plan closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-native-plan-closure-20260906.md).

**Spec181 native source diagnostic R3 (2026-09-06): BLOCK at DI projection declaration.**
A checkout refusal left the first native retry on the prior HEAD plus an
explicit source patch; that run is invalid as clean-commit evidence. It
terminated with two compiler errors: NativeCanonicalOnnxAssembler reads
canonicalArtifactName absent from the committed NativeSelectionProjectionV3.
The nine tested files were then matched to 1df718c8 and checkout completed.
Inspect the declaration and assignment path before a fresh recorded retry.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 framework source closure R1 (2026-09-06): BLOCK at assignment metadata.**
The isolated six-file dependency closure builds the production framework
library. Its reused lifecycle/assignment checks yield 16 PASS / 1 FAIL:
ServiceProvider loses artifactDataName while projecting a structured
assignment set into CollaborationContext. Preserve the R1 source patch and
result, then close the exact Provider transfer before retrying.
R2 carries the root name through single/structured assignments and rejects
conflicting roots. The isolated target links and passes 26 cases / 204
assertions. This framework boundary is closed; full native closure remains.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 committed native build R4 (2026-09-05): BLOCK at C++ source closure.**
Clean 6c7a0b23 passes configuration and Waf graph creation, then ServiceUser.cpp
fails to compile: AckAuthenticationEvidence is missing, followed by missing
registration and publish-result declarations. The implementation depends on
uncommitted framework declarations/companions. Preserve R4 and close those
exact dependencies before the next clean build; no protocol result exists.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 committed native build (2026-09-05): BLOCK at Waf graph creation.**
Detached d67de87a configures successfully, but tests/wscript references an
untracked native assembly integration source. find_node returns None and Waf
fails before C++ compilation. No protocol result exists; preserve the isolated
checkout and repair the committed source dependency before retrying.
One focused identity regression also fails: changing loaded tests/wscript
bytes with unchanged mtime does not invalidate the native receipt, because
that Waf control file is missing from source fingerprints.
R2 binds the missing control file (70 identity checks PASS) and builds the
integration target, but its seven named assembly cases yield 3 PASS / 4 FAIL.
All four fail at certified recipe_digest validation before ORT loading;
preserve R2 and compare fixture serialization with the production contract.
R3 corrects the old fixture's quoted integer dimensions, leaving production
digest validation intact: 7 cases / 160 assertions PASS. The missing test
source and identity repair are ready for a checkpoint; a fresh committed
checkout must still pass the maintained native build. T007 remains BLOCK.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 A05 qualification inventory (2026-09-05): CLOSED within focused inventory repair.**
The inherited inventory requires Q-C/Q-W but discovers only Spec180 Python
tests, omitting Spec181 protected-grant regressions. Two tests using the real
inventory builder and pytest collection fail; no formal network run started.
R2 repairs those cases (15 PASS); an old count assertion is corrected, and R3
passes 16 checks. R4 then exposes a second boundary: two tests show rehashed
case-source substitution is accepted by the real builder and gate. These use
fixture children, not a network qualification. Preserve R4 before repair.
R5 rejects both substitutions (17 PASS); the old missing-oracle fixture also
changed its source path and is now correctly rejected earlier. R6 keeps the
registered path while withholding its oracle to preserve that check's scope.
R6 passes 18 checks. R7 then finds three unsafe entry IDs accepted by the
validator although the gate joins IDs into output directories; no escaped
write is attempted. Reject these IDs at the inventory boundary before R8.
R8 passes all 21 checks: active scope/collection, registered case source/args,
safe entry IDs, existing evidence/oracle controls and wrapper compatibility.
The remaining A05 native/candidate effective-configuration audit stays BLOCK.
See [qualification scope repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-qualification-scope-20260905.md).

**Spec181 T007 evidence inventory (2026-09-05): CLOSED within document inventory scope.**
The R003 record reports zero Spec181 evidence files, while the current tree
contains 37. Four active evidence records lack an explicit header layer, and
the audit body still describes already-closed T002/shared-runtime gaps. This
invalidates the old completeness claim, not the linked raw test results.
Four layer headers are repaired; the current inventory covers 145 entries,
including both audit roots and one explicit SELF row. Drift/structure checks
PASS, and all 105 Spec180 evidence-file hashes match the before-scan. Raw scan:
ignored workspace temporary directory `spec181-evidence-inventory-20260905-r1/`.
See [complete inventory](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-evidence-inventory-20260905.md).

**Spec181 shared generation worker (2026-09-05): CLOSED by focused repair.**
Two queued coordinator regressions entered the model after cancellation or
deadline. The existing pre-run cancellation check passed (1/3 cases PASS,
14/18 assertions PASS). Propagating the shared guard through the registered
runtime/worker and state staging repairs the boundary: rebuilt 48 cases /
366 assertions PASS. T007 remains BLOCK for its remaining audit obligations.
See [generation worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-generation-worker-20260905.md).

**Spec181 shared preparation control R1 (2026-09-05): CLOSED as startup error; R2 focused PASS.**
The isolated launcher omitted the empty output directory; `validate_inputs`
raised `OUTPUT_ROOT_MISSING` before MiniNDN startup. No protocol result exists.
The unified native build passed. R1 is preserved; fresh R2 passed the protected
P-256 control with four verified Providers and seven collected child exits.
See [shared preparation closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-shared-preparation-20260905.md).

**Spec181 T002 native handler observation (2026-09-05): CLOSED; early result INVALID.** A focused test
was started before the repair build completed and ran the previous binary.
R2/green.log is preserved. The original build completed (59.612s), then the rebuilt
binary passed 46 cases / 242 assertions in a new log. The early result is a
validation orchestration error, not a protocol result.
See [native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native prepared output binding (2026-09-05): CLOSED (focused repair).** The production
prepared-runner validator accepts a Merge output budget changed from the sealed
Selection's K=300 to K=1. R1 fails one of two assertions after a passing positive
control. Exact output shape/type binding now rejects the mutation; rebuilt focused
checks pass 46 cases / 242 assertions. Source closure remains pending; see
[native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native Merge contract (2026-09-05): CLOSED (focused repair).** Direct production-runner
checks pass the numerical controls but expose four missed rejections: unknown
postprocess identity, numeric suffix, trailing shape delimiter, and wrong output
dtype. R1 is preserved (3/6 cases, 22/26 assertions passed). Rebuilt repairs pass
6 cases / 26 assertions; related evidence/readiness checks total 10 cases / 69
assertions. Handler source closure remains open; see
[native Merge closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-merge-closure-20260905.md).

**Spec181 T002 native worker authority (2026-09-05): CLOSED (focused repair).** Four real-worker
regressions show that cancellation/expiry after preparation or during compute
still returns results and retains the registered plaintext lease. Grant
verification is valid initially; the missing boundary is worker consumption.
R1 is preserved. Request guards now fence preparation, compute, cached results,
events, publication, and return; rebuilt focused checks pass 51 cases / 280 assertions.
T002 source closure and unified production rebuild remain open; see [worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-worker-authority-20260905.md).

**Spec181 T001 request lifecycle (2026-09-05): CLOSED.** Twelve registered-handler
regressions show that cancellation or Selection deadline expiry before/during
grant fetch or after preparation still reaches model execution. Grant expiry
alone does not enforce request lifetime. Preserve the first red run before
repair. Four final regressions also expose a missing comparison between the
grant-reference and Selection policy snapshot. Both repairs pass 151 focused
regressions and six real Python process cases (24 exits collected). All red
runs are preserved. T001 acceptance is complete; T002/T007 remain open. See
[request lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-request-lifecycle-20260905.md).

**Spec181 T001/T002 maintained process integration (2026-09-05): focused defects CLOSED.**
R1 native fixtures build, but NFD's own management FIB registration fails
because the test configuration omits management authorization. The requester
then receives connection refused; no grant verification occurred. Preserve
the r1 raw run. R2 fixes NFD but requester bootstrap requires a real Controller
for NAC public parameters. Separately, ten successful P-256 runtime calls leak
24560 bytes / 630 allocations under ASAN despite the Zeroized state. Both
boundaries are recorded before repair. R3 fixes the EC ownership leak: ten
P-256 calls pass ASAN. The real Controller becomes ready, but requester
publication readiness times out before any Provider result; preserve r3
before instrumenting that boundary. R4 NDN logs/stack locate the wait in
ServiceUser construction (NAC decryption key): the fixture needs its own
requester policy and certificate bootstrap. R5 passes five cases; Python's
wrong-recipient rejection is correct, but the test incorrectly requires a
Core wire prefix on an internal typed exception. Preserve r5 and correct
the scoped assertion without synthesizing a Core response. R6 passes all 11
checks (10 real-process cases plus ASAN), with all 40 child exits collected.
T001/T002 full acceptance and T007 remain open. See
[process integration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-t002-process-integration-20260905.md).

**Spec181 T002 P-256 production path (2026-09-05): focused defects CLOSED.**
R1 proves the runner overwrites an explicitly configured recipient-key map
with the Ed25519 offer-key map. Four other checks stop in fixture key
generation because this cryptography installation requires an explicit backend;
they do not prove a production refusal. R2 fixes the fixture: three actual
entry regressions fail (requester loader, Python Provider loader, map override),
and two wrong-curve rejection checks pass. R3 repairs those entries: 130 checks
pass, but the positive P-256 case reaches the production envelope creator and
fails because its EC key generation also omits the required backend argument.
All three raw results are retained. R4 repairs the production key generation;
134 focused checks pass, including bounded private-file rejection. The unified
native rebuild and a fresh four-recipient P-256 Y-B control pass: four native
grant verifications, terminal numerical match, seven collected child exits,
and empty staging. Full T001/T002 acceptance remains open.
See [P-256 production path](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-p256-production-20260905.md).

**Spec181 T002 recipient credentials (2026-09-05): focused defect CLOSED.**
The production factory accepts only Ed25519 private keys although T002 and
the native verifier also support EC P-256 envelopes. The rebuilt credential
regression runs eight cases; only P-256 loading fails before grant acquisition
with `provider recipient private key is not Ed25519`. The r1 build and RED
logs are retained. R2 loads validated P-256 PEM through the production loader;
all eight focused checks pass, including wrong-curve and permission rejection.
P-256 network acceptance and full T002 closure remain open. See
[recipient credentials](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-recipient-credentials-20260905.md).

**Spec181 T002 helper lifecycle (2026-09-05): focused lifecycle defects CLOSED.**
The native assembler waits synchronously for its helper, ignores request and
operation deadlines during assembly, observes grant cancellation/expiry only
after slow work, and can activate helper output beyond the role envelope.
Six real-process regression failures are retained in
`spec181-t002-helper-20260905-r1/red.log` in the ignored workspace temporary
directory. R2 builds but 14 native checks stop at the loader: the old installed
framework lacks `streamCancelled`. R3 binds the test executable and its
environment to the current build library: 26 focused checks pass. A new
source-fetch cancellation regression then proves that the parent recreates
the erased plaintext directory. R4 serializes protected staging writes
with runtime cleanup; 27 focused checks pass, including the new race. The
new RED log is retained in r3. The final unified native rebuild and a fresh
protected Y-B control pass: four actual grant verifications, three ORT CPU
roles plus native Merge, verified terminal output, and empty staging after
all seven child exits are collected. Full T002 acceptance remains in progress. See
[T002 helper lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-helper-lifecycle-20260905.md).

**Spec181 T003 assembly parity (2026-09-05): focused build defects CLOSED.**
The fixed-vector Python lane passes 8 cases; the native lane fails 8 checks
because its required current-source test executable is not built yet. This
is a test-input boundary, not an assembly or protocol rejection. R2 compile
also fails before execution because the manual command omitted the installed
NAC-ABE package's `NAC_ABE_CMAKE_BUILD` definition. R3 also lacks the maintained
framework include path; r4 replaces the manual command with a focused Waf
target using the existing dependency configuration. R4 compiles but exposes
the framework's NDNSD link dependency; r5 adds that configured dependency.
R5 build passes; the final 19 parity checks pass, including real ORT CPU
execution and unchanged negative-cache state. Fixed-vector regeneration is
byte-identical. T003 is complete at its focused scope; T007 remains BLOCK. Preserve
`spec181-t003-assembly-20260905-r1` and build the production-entry fixture
before another attempt. See
[T003 assembly parity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t003-assembly-parity-20260905.md).

**Spec181 T005 evidence preservation (2026-09-05): focused defects CLOSED.**
Five focused checks expose the legacy driver's destructive attempt handling,
runtime-dependent entry, and continuation after matrix failures. No network
processes are started. The repair retires this unsafe automatic retry path
and makes the maintained matrix stop at its first failed subcase. Raw RED
output is retained under `spec181-t005-evidence-repair-20260905-r1` in the
ignored workspace temporary directory. R2 passes 118 focused checks after
retiring the legacy entry and stopping on the first matrix failure. This is
not formal matrix qualification; T005 remains NOT PROVEN. See
[T005 evidence repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-evidence-repair-20260905.md).

**Spec181 T006 positive control (2026-09-05): cold dependency timeout CLOSED.**
All four native grants verify, but r8 Merge's first tensor-manifest fetch
expires at the fixed 10 s no-progress bound while its producer finishes cold
model preparation. User then times out. The three actual grant negatives
pass. After binding the data wait to the configured request budget while
retaining the existing hard deadline and cancellation, r10 completes the
protected native control; a measured dependency wait is 13.97 s. The final
r11/r12/r13 negatives also pass with complete process collection. T006 is
complete at its focused scope; T007 remains BLOCK. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).

**Spec181 T006 production rejection (2026-09-05): false-positive oracle CLOSED.**
Eight focused regressions fail: the configured requester seam publishes no
mutation, invalid mutation/epoch settings are admitted, and User-local
exceptions or markers can masquerade as Provider rejection. No selected
Provider network rejection is established by those old probes. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).
After repair, 62 focused checks pass. The separate runner check retains one
obsolete expectation that a User-local Y-N-E probe should report PASS; that
test is corrected; the updated Python group has 150 PASS. A separate C++
harness link omitted the store source; the next command used a nonexistent
shortened filename. The verified source is NativeProtectedArtifactStore.cpp;
use a new directory for the corrected harness command. The unified native
build has independently completed successfully.
r4 C++ harness passes 22 cases. The r5 live entry stops before network
creation because Y-B inputs omit Y-N's FullModel role. Use the verified
five-role Y-N inputs with an explicit protected epoch for subsequent variants.

**Spec181 T004 lifecycle acceptance (2026-09-05): focused defects CLOSED.**
R1 failed before the waiter because the fixture had no running Controller
serving AA public parameters. R2 corrects that startup order and reaches the
real Provider waiter: an 80 ms wait returns false in about 6 us before run,
with SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL. The initial non-running
state was incorrectly terminal. R3 retains an OUTPUT_ROOT_MISSING preflight
failure. After the native fix/rebuild, r6 waits 83 ms and starts/stops the real
Provider; r4 reaches Controller readiness at 12.39 s; r5 cancels an active
Core probe in 2.3 ms without hot spinning. All three probes exit 0 after
process collection and network cleanup; six Core checks also pass. T004 is
complete at its focused acceptance scope; T007 remains BLOCK. See
[T004 lifecycle acceptance](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t004-lifecycle-acceptance-20260905.md).

**Spec181 T002/T004 native repair (2026-09-05): focused defects CLOSED; tasks OPEN.**
Retained failures identify group digest format, grant forwarding hint, model
basename, premature readiness, and debugger exit-code boundaries. Live-r7
uses ordinary process commands and the same rebuilt source: native protected
Y-B returns PASS/exit 0, with three ciphertext files and no ONNX plaintext or
staging remnants after cleanup. Production negatives, cancellation/resource
acceptance, source checkpoint closure and T007 remain open. See
[native launch repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-live-repair-20260905.md).

**Spec181 T002 production wiring (2026-09-05): build defect CLOSED; T002 OPEN.**
The first compile failed on the installed ndn-cxx forwarding-hint API. A
separate retained r2 build passes native/library/extension identity checks,
and 3 rebuilt-extension tests consume the 9 grant vectors. Real Provider
network and ORT lifecycle acceptance remains open. See
[T002 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-production-repair-20260905.md).

**Spec181 T002 native runtime repair (2026-09-05): focused defects CLOSED.**
18 native focused tests pass with real verification, managed content keys,
deadline/cancellation checks and retryable cleanup. Initial compile, linker
and consumption/deadline failures remain preserved. Factory, storage AEAD
and real network acceptance still keep T002 open.
See [T002 runtime repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-runtime-repair-20260905.md).

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

The `r35` result is the newest active blocker. Its P/E logs show the
Controller issuing `registering prefix: /example/controller` and the six
NDNSF filters, but no successful Controller-prefix registration and no
`PUBPARAMS` filter before the Python readiness timeout. This is classified as
a startup/environment boundary failure, not as an ACK disposition failure.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
