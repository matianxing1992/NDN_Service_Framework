# NDNSF-DI Runtime Workflow

## Operator handoff (2026-08-19)

Before changing NDNSF-DI or producing a new SIF, read the concise current
baseline and failure map in
[`spec170-update-lessons-20260819.md`](../specs/170-reusable-layer-artifacts/evidence/spec170-update-lessons-20260819.md).
It records the only currently supported local-Apptainer-to-Tiger route, the
usable r23 image and its source boundary, the fixed toolchain/runtime values,
and the recurring errors that must stop an update before another multi-GB
build. This handoff is an index, not a replacement for the detailed gates
below.

The source-to-SIF update contract below is the first step when NDNSF-DI source,
dependencies, native bindings, or the Apptainer definition changes. The
profile workflow later in this document is the runtime-entry reference after
an immutable candidate has been selected.

## Mandatory design-to-code convergence gate

Before accepting any complete unit/integration suite, MiniNDN run, SIF
qualification, TigerCluster job, benchmark, or experiment as evidence, first
audit the intended design against the implementation. Freeze the active
`spec.md`, `plan.md`, contracts, invariants, and effective configuration, then
trace the real production entry points and callers with CodeGraph and exact
source inspection. Check the runtime wiring, security checks, dependency and
configuration resolution, terminal ownership, cleanup, logging, and evidence
paths. A helper, passing isolated test, or checked task does not prove that the
production path uses the intended mechanism.

Record every gap with severity, source evidence, affected requirement,
correction owner, and a closing focused regression. If the written design is
incomplete, correct the specification and task order before repairing code;
otherwise repair the production path to match the accepted design. Focused
red/green tests and the smallest bounded reproducer are allowed during this
repair loop. Formal validation is blocked while any semantic, architecture,
security, production-wiring, cleanup, observability, or evidence-validity gap
remains. Re-run the audit and require a fresh `PASS` before qualification.

This gate also applies before planning or scheduling a broad test: first trace
the intended production path and repair or record every design/code gap. Do not
use a planned unit, integration, MiniNDN, benchmark, SIF, or Tiger run to
discover the design distance; only a bounded focused regression for a named
repair finding may be scheduled while the verdict is `BLOCK`.

Any later behavior-affecting source, dependency, model/artifact, launcher,
topology, effective-configuration, harness, or evidence-schema change
invalidates that `PASS` and requires a new audit before the next formal run.
Use the canonical checklist at
[`design-code-convergence.md`](../.specify/memory/design-code-convergence.md)
and the active feature's audit/evidence record. For Spec175, an unresolved
`BLOCK` in `evidence/design-code-audit-*.md` is a hard stop: do not reseal a
candidate, build another large image, allocate Tiger resources, or report a
benchmark result as qualification evidence.

### Per-run sign-off card

Copy this card into the current evidence record before accepting a run. Every
box must be checked for the exact source/build/configuration subject; one
unchecked box keeps the subject at `BLOCK`, even if the command itself exits
successfully:

```text
[ ] Design authority and effective configuration are frozen and identified.
[ ] CodeGraph plus exact source inspection trace the production entry points,
    callers, wiring, dependencies, security, cleanup, logging, and evidence.
[ ] Each design/code gap has severity, owner, correction, and a focused
    closing regression; unresolved controlling gaps are recorded as BLOCK.
[ ] The repaired production path was re-inspected and this exact subject has
    a fresh code-aware PASS.
[ ] The run uses the audited executable, libraries, interpreter, artifacts,
    working directory, arguments, environment, and topology.
```

Focused red/green tests are allowed only to close a named gap in this card.
They are development evidence and do not waive the card or unlock a complete
unit suite, MiniNDN qualification, SIF replay, Tiger job, benchmark, or paper
result. Record the audit identity and card before starting the command, not
after inspecting a convenient result.

### DI logging filters

Spec175 application processes use one bounded, component-selective filter in
both the local and Tiger launchers:

```text
*=WARN:ndn_service_framework.TimelineTrace=WARN:ndnsf.di.RuntimeEvidence=WARN
```

It preserves WARN-level machine-readable timing records and suppresses routine
trace/info output. A focused repair run may temporarily use
`*=WARN:ndn_service_framework.TimelineTrace=WARN:ndnsf.di.RuntimeEvidence=TRACE`
for the DI component only. The launcher passes this through each application
environment; never put an application filter in the host-wide environment that
NFD inherits.

## Current SIF update decision (2026-08-19)

An existing SIF is not a mutable runtime environment. It is usable only for
the exact source seal, dependency lock, Apptainer definition, native-library
hashes, and Python-extension hashes recorded beside that SIF. A successful
`apptainer exec`, import, or old Tiger smoke therefore answers only “does this
sealed candidate run?”; it does not qualify a newer NDNSF-DI checkout.

Use this decision before every update:

```text
source/lock/native/definition changed?
  no  -> reuse the recorded SIF; create only a new bundle/manifest when the
         externally mounted workload or model changes
  yes -> keep the old SIF as a control, create a new candidate ID, and rebuild
         one complete local SIF; never copy files into or patch the old image
```

The current tested configuration is deliberately fixed while this loop is
being repaired: the explicit local Apptainer binary
`/opt/apptainer/1.5.3/bin/apptainer` and allocated-compute Apptainer `1.5.3`,
`build-local-sif.sh` as the only release entry point, Python 3.10 and
`/opt/venv/bin/python` inside the SIF builder, explicit
`BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` closure on every Provider/ONNX target,
one hash-bound SIF, one read-only bundle, external content-addressed model
artifacts, `cd "$BUNDLE"` before Provider launch, and isolated HOME/PIB per
role. TigerCluster only verifies, stages once, and executes; Docker, OCI
conversion, registry pulls, and Tiger-side materialization are not part of the
current route.

The current qualification boundary is also explicit: r23 is a physically
usable image for its sealed commit, while the working tree is not yet sealed
for a new candidate. Current-tree diagnostics (496 C++ unit cases, 34 C++
integration cases, and the latest 105-pass Python Spec170 contract glob) do
not change that boundary.
The auxiliary authorization/security run is 77/78 because eleven registered
source hashes differ from the older frozen inventory; do not edit the frozen
inventory merely to obtain a green test. Select and review the intended source
candidate, regenerate its source seal/inventory, then build a new SIF before
using those changes on Tiger. Until the T029 freeze and remaining lifecycle
negative gates close, label r23 **bounded verification**, not a release.

### Current update checkpoint (2026-08-19)

Apply this short card after every NDNSF-DI source update and before allocating
Tiger or creating another multi-GB image:

| Check | Required current rule | Stop condition |
|---|---|---|
| Candidate identity | Create a new source/lock/definition/bundle ID and regenerate the candidate-input inventory | An intended source file is missing, or the inventory changes after sealing |
| Build driver | Use `/opt/apptainer/1.5.3/bin/apptainer` and `build-local-sif.sh` | Ambient Apptainer, Docker/OCI materialization, or a Tiger-side build |
| ABI boundary | Build `_ndnsf.so`, Provider binaries, and all native code inside the SIF builder/sealed ABI | Host `Python.h`, host venv/site-packages, host `.so`, or mismatched `SOABI`/`EXT_SUFFIX` |
| Native closure | Audit every Provider/ONNX sibling target for `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` and run the in-SIF library lock | One target passes while a sibling has unresolved NDN/ONNX/Boost symbols or host `ld` contamination |
| Model/runtime | Compare canonical ONNX graph/initializer, standalone tokenizer, adapter metadata, and hashes with the exact image lock; run an ONNX Runtime load/conformance probe before MiniNDN. Any PyTorch/Transformers conversion is offline-only | The deployment SIF contains/imports PyTorch/Transformers, or only a host/temporary package overlay makes the canonical ONNX artifact load |
| Bundle | Materialize every relative artifact and its `.sha256` sidecar, then verify it from `cd "$BUNDLE"` | Missing artifact, wrong cwd, or an unverified relative path |
| Runtime | Use one read-only bundle, isolated HOME/PIB, and Controller → bootstrap Provider → User barrier | READY without Request → ACK_CLOSED → Selection → Response evidence |
| Cleanup | Start Providers with `exec env ... di-native-provider`, reap tracked PIDs, and run a zero-process census | Any job-local Provider remains after the workload |
| Promotion | Hash the local SIF once, compare record/remote/staged hashes, and let Tiger verify/stage/execute only | Hash drift, a stale source seal, or a second large image created to repair a failed candidate |

The current r23 image remains usable for its recorded sealed source only. A
recent combined D0→D1 repeat also exposed an evidence-quality warning: the
`backbone-to-head0` producer timing line once reported `bytes=0` and an empty
planned name even though the consumer fetched 120 bytes and the final response
succeeded. Do not relax the dependency analyzer to hide this mismatch; classify
it as a producer-side timing/instrumentation race. The parser now uses the
actual `data_name` URI when Python emits `planned_name=true|false`, and the
current source serializes the native timing block; r23 predates that C++ fix.
Reproduce it in the next source-bound SIF before calling the lifecycle evidence
deterministic.

The update loop is fail-closed and must be recorded in the candidate evidence
directory in this order:

```text
disk/retention and exact disposable-path census
  -> candidate-input inventory + new source/lock/definition identity
  -> target compute-node Apptainer parity
  -> source-seal and build-boundary validation
  -> complete Waf target census (all sibling targets)
  -> one local complete SIF
  -> exact-SIF Python/native/ldd/library closure and stale-file census
  -> real CPU/MiniNDN Request -> ACK_CLOSED -> Selection -> Response
     plus negative authorization/replay/tamper/partial cases
  -> one hash-bound promotion
  -> Tiger hash verification, stage-once, and execute-only
```

The first source-side artifact for a new candidate is the deterministic input
inventory. Generate it before sealing or building:

```bash
python3 tools/ndnsf-di/collect_spec170_candidate_inputs.py \
  --workspace . \
  --output "$EVIDENCE/candidate-input-inventory.json"
```

It binds the runtime source, native/Python bindings, examples, packaging and
build inputs, relevant tests/harnesses, and Spec170 contracts to one digest.
The inventory is intentionally pre-freeze: it does not replace T029's frozen
manifest or the separate model, security/route/schedule, Gate A/B/C, and SIF
records. A missing intended input or post-seal inventory change is a hard stop;
select a new candidate rather than patching or reusing the old image.

The recent failures are prevention rules, not reasons to add another ad-hoc
wrapper: host Python headers or host `_ndnsf.so` means `WRONG_BUILD_BOUNDARY`;
a sibling link failure means the target's own Waf `use=` closure is incomplete;
an old extension in a base SIF means stale-base replacement failed; a missing
relative artifact means either the Provider did not enter `$BUNDLE` or the
generator failed to materialize the manifest-referenced file and its hash
sidecar into the bundle; a Provider wrapper that does not `exec` the child can
leave orphaned Providers after cleanup; READY without a response means the
startup/lifecycle gate is incomplete; a large-fetch stall
means the Face event loop or bounded fetch contract is wrong; Linuxbrew/host
`ld` pulling GTK/UAV libraries means `HOST_TOOLCHAIN_CONTAMINATION`; and a
readable SIF with a different source/native hash is stale. Each repair gets a
new candidate identity and preserves the failed evidence.

The historical model/runtime finding is recorded in
[`spec170-qwen3-toolchain-preflight-20260819.md`](../specs/170-reusable-layer-artifacts/evidence/spec170-qwen3-toolchain-preflight-20260819.md):
r23 (Transformers 4.48.2) and the host child (4.46.3) rejected the pinned
Qwen3 architecture before NDNSF started. That incident explains why the
Transformers path was introduced as a diagnostic/exporter detour; it is not the
deployment contract. A current Qwen3 candidate must instead seal canonical ONNX
graph/initializer/tokenizer objects plus adapter conformance metadata, pass the
ONNX-only runtime scan, and complete dependency closure in a new local SIF
before Gate B. A temporary Transformers overlay remains diagnostic evidence
only, never a release input.

### Provider launch and cleanup contract

Every workload that starts a Provider must use the following shape:

```bash
(
  cd "$BUNDLE"
  exec env "$provider_pib" "$provider_tpm" \
    /opt/ndnsf-di/current/bin/di-native-provider ... --serve
) >"$LOG/provider.log" 2>&1 & PIDS+=("$!")
```

`cd "$BUNDLE"` makes relative manifest artifacts deterministic. `exec` makes
the PID stored in `PIDS` the Provider itself rather than an intermediate shell;
the existing `TERM`/`KILL` cleanup then actually stops and reaps every child.
After each exact-SIF workload, check that no job-local `di-native-provider`
remains. A passing response does not qualify a workload that leaks serving
processes or leaves an allocation dirty.

### Update record that must travel with the candidate

Do not keep this procedure only as prose. Before a new build, create the
candidate evidence directory and retain these files together:

```text
preflight.json                 disk, paths, source/lock/definition identity,
                               Apptainer parity, target census, and PASS/FAIL
candidate-input-inventory.json deterministic candidate input list, per-file
                               hashes, worktree status digest, and inventory digest
build-record.json              SIF size/hash, containerNativeBuild,
                               hostBinaryInputs, Python ABI, and toolchain
sif-library-closure.json       Provider/_ndnsf.so plus every packaged .so
source-seal.json               exact source and dependency boundary
local-tests.txt                C++/Python/MiniNDN positive and negative gates
promotion.json                 remote/staged hash and tigerAction
failure-<class>.json           first failed run, before the repair
```

The evidence directory is the handoff between local build and Tiger. A later
import, `READY`, or successful smoke cannot replace a missing preflight row or
erase the first failure. If the candidate changes, create a new directory and
candidate ID; never overwrite the old record.

Source-only candidate preparation must also be content-addressed: the seal
digest must not change merely because the workspace or archive is written under
a different absolute directory. New seals use
`sealDigestBasis=path-independent-content-v1`; the validator still accepts
legacy path-bound v1 seals so existing candidates are not silently rewritten.
The regression and two-directory proof are recorded in
[`source-seal-determinism-20260819.md`](../specs/170-reusable-layer-artifacts/evidence/source-seal-determinism-20260819.md).

For the native `NDNSF_DATA_V1` path, keep the normal group no-progress bound at
2000 ms. A fault-injection run that deliberately inserts a longer stage gap
must pass an explicit `--data-v1-no-progress-ms` value and record it in the
summary; do not silently raise the bound or change the global deadline. The
post-certificate cancellation result and delayed-stage diagnosis are recorded
in [`real-minindn-cancellation-filterfix-20260819.md`](../specs/170-reusable-layer-artifacts/evidence/real-minindn-cancellation-filterfix-20260819.md).

Do not start a multi-gigabyte build until this preflight is PASS. With limited
local disk, retain the active SIF, its record/source seal, hashes, and canonical
evidence; enumerate and remove only named superseded paths after recording
their sizes and verifying free space. A temporary SIF is never a second release
candidate.

## Source-to-SIF update contract (Spec170)

The only current release route is **local Apptainer build → exact-SIF closure
checks → hash-bound TigerCluster execution**. Docker, OCI archives, registry
pulls, and Tiger-side materialization are historical diagnostics and must not
be mixed into a current candidate.

The current sealed candidate is `runtime-r23.sif`. It is a usable bounded
verification image for the exact committed source revision in its build
record, but it is not a T029/T036-frozen release and does not qualify the
uncommitted working tree. Keep this distinction explicit: a physically usable
SIF is not automatically a current-source SIF.

```text
path: .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
sha256: 5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
record: .codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
sourceRevision: 989a9daace669a4f93496dade3176c527edb2469
sourceSeal: sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e
bytes: 4442767360; apptainer: 1.5.3; containerNativeBuild=true; hostBinaryInputs=[]
tigerAction=verify-hash-and-execute-only
sealedSourceStatus=USABLE_FOR_RECORDED_SOURCE_ONLY
evidence=specs/170-reusable-layer-artifacts/evidence/current-sif-r23-local-20260819.md
```

The exact-image checks recorded on 2026-08-19 passed: Python 3.10.18 and
`import ndnsf._ndnsf`, one active `_ndnsf*.so`, zero unresolved extension
dependencies, and the Provider `--check-only --wiring-check-only` command on
the read-only r23 D2b bundle. The Provider check is only a plan/wiring gate;
it does not replace the real CPU/MiniNDN lifecycle, negative cases, library
lock, or Tiger promotion gates.

The post-cleanup reuse gate was rerun on the same file: `sha256sum` matched
the record, `validate-local-sif-build-record.py --metadata-only` returned
`status=PASS`, and an import inside the exact SIF printed
`NDNSF_SIF_IMPORT_OK`. At that check the host filesystem had 75 GB available
(56% used), after the explicitly recorded Apptainer-cache cleanup. The durable
record is
[`current-sif-reuse-gate-20260819.md`](../specs/170-reusable-layer-artifacts/evidence/current-sif-reuse-gate-20260819.md).
This confirms r23 is reusable for its sealed source only; the dirty checkout
still requires a new candidate before execution.

Before reusing or promoting any candidate, run the cheap identity gate below.
It deliberately performs the expensive SIF hash only once, then checks the
record without reading the image again. A failure means “stale or incomplete
candidate”; do not repair the image in place or start a Tiger job.

```bash
SIF=.codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
RECORD=.codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
EXPECTED=sha256:5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb

df -h "$PWD"
test -r "$SIF" -a -r "$RECORD"
test "sha256:$(sha256sum "$SIF" | awk '{print $1}')" = "$EXPECTED"
python3 packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/\
  validate-local-sif-build-record.py \
  --record "$RECORD" --sif "$SIF" --expected-sha256 "$EXPECTED" \
  --metadata-only
/opt/apptainer/1.5.3/bin/apptainer exec "$SIF" /opt/venv/bin/python \
  -c 'import ndnsf, ndnsf._ndnsf; print("NDNSF_SIF_IMPORT_OK")'
```

The hash/record/import checks prove only the recorded source. Compare the
recorded source seal and native hashes with the checkout that will run; any
C++, Python, NDN-SVS/NDN-CXX, dependency, definition, or lock change requires
a new candidate ID and one new complete local SIF. Do not create a second
multi-gigabyte image merely because the old candidate is readable.

### Current no-repeat configuration

The current SIF route is intentionally simple and must stay that way:

```text
Apptainer        local 1.5.3 == target compute 1.5.3
build            build-local-sif.sh only
native ABI       /opt/venv/bin/python (Python 3.10) inside the SIF builder
Waf closure      BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL on every target
runtime          one SIF + one read-only bundle; Provider starts from $BUNDLE
identity         isolated HOME/PIB and identity per Controller/Provider/User
cluster          hash verify, stage once, execute; no Docker/OCI/materialize
disk             record `df -h` every run; 75 GB was free after the
                 2026-08-19 cache cleanup (53 GB before cleanup); no duplicate
                 multi-GB image or model copy
```

The phrase “usable SIF” has two required meanings: the image passes its own
sealed-source checks, and its source seal/native hashes match the source about
to be tested. r23 satisfies both meanings for commit
`989a9daace669a4f93496dade3176c527edb2469`; it does not include later
uncommitted working-tree changes. Never infer source parity from a successful
import, READY marker, or old Tiger run.

The repeated incidents are now explicit preflight stops: host-built
`_ndnsf.so` or missing `Python.h` (`WRONG_BUILD_BOUNDARY`), fixing one Waf
target while a sibling misses `NDN_CXX`/`NDN_SVS`/ONNX Runtime, stale native
files inherited from a base SIF, relative artifacts opened without
`cd "$BUNDLE"`, shared PIB/bootstrap races, a stopped Face event loop during
large fetches, and Docker/OCI or duplicate-SIF disk pressure. A local import,
READY marker, or GPU-visible probe cannot waive any of these checks.

### Host-runtime rebuild boundary for MiniNDN diagnostics

MiniNDN diagnostics run with the host Python runtime and are not evidence that
the container ABI is correct. After changing `ServiceUser`, the framework
shared library, or another C++ binding dependency, rebuild the host diagnostic
stack before interpreting a MiniNDN result:

```text
source change
  -> ./waf build --targets=ndn-service-framework -jN
  -> (cd pythonWrapper && python3 setup.py build_ext --inplace --force)
  -> verify extension timestamp/hash and ldd against the rebuilt framework
  -> run focused C++ and Python tests
  -> run the real MiniNDN lifecycle/negative gate
```

The host-built extension may be used only for this host MiniNDN diagnostic. It
must never be copied, bind-mounted, or used as input to a candidate SIF. A SIF
candidate follows the separate container-runtime boundary: rebuild the native
library and `_ndnsf.so` inside the SIF builder, then repeat import, ABI,
RPATH/`ldd`, library-lock, and real lifecycle checks inside that exact SIF.
An old host extension or shared library can otherwise produce a false
diagnostic failure even when the source fix is present, while a host import can
also falsely qualify an invalid SIF.

The current post-rebuild evidence is intentionally separated: two real CPU
MiniNDN positive hybrid cases passed, but the Spec111 post-certificate
cancellation gate remains negative. With the default 5 s lease timeout, the
run reached `LEASE_CAPACITY_REJECTED`; with a diagnostic 20 s lease timeout,
lease acquisition succeeded but providers returned `CANCEL_REJECTED` after
native execution had started. These are open negative-gate failures, not
reasons to weaken the SIF or silently call the candidate PASS.

The host linker is also not an acceptance authority. If Linuxbrew or another
host toolchain resolves unrelated GTK/UAV/system libraries, reports an
incomplete `.so` closure, or produces a binary whose dependency roots are not
inside the sealed builder, classify it as `HOST_TOOLCHAIN_CONTAMINATION` and
stop. Do not turn a partial host link into a “full build” result and do not
repair it by copying the host binary into the SIF. The repair is to rebuild the
complete target set inside the candidate SIF (or its sealed ABI-identical
builder) and rerun the internal closure gate.

### When a new SIF is required

| Change | New SIF? | New bundle/manifest? |
|---|---:|---:|
| C++, Python wrapper, Provider, framework, NDN-SVS/NDN-CXX, native extension | **yes** | yes |
| dependency lock, compiler, Python ABI, Apptainer, definition, library RPATH | **yes** | yes |
| model/runtime library packaged inside the image | **yes** | yes |
| only workload, policy, route, prompt, or externally mounted model | no | **yes** |
| documentation/evidence only | no | no |

Never copy a new `.so` into an old SIF or bind a host virtual environment.
Every candidate has one source seal, lock, definition, SIF, build record, and
bundle identity. A source/runtime change creates a new candidate ID.

### Required sequence after an NDNSF-DI update

1. Record a new candidate ID, source/lock/definition digests, target partition,
   and available disk. Keep the active SIF and evidence; clean only named
   disposable paths after recording their paths and bytes.
2. Query Apptainer inside a bounded allocation on the target compute partition
   and use the same semantic version locally (the qualified pair is 1.5.3).
   Invoke the recorded binary explicitly as
   `/opt/apptainer/1.5.3/bin/apptainer`; do not trust an ambient `PATH`
   entry. On the verification host, `/usr/local/bin/apptainer` reported 1.3.4
   and was not used for the r23 evidence. Login-node output is diagnostic only.
   Host Python headers, venvs,
   site-packages, and native libraries are never build inputs.
3. Run source-seal/build-boundary validation and a complete Waf target census.
   Every ONNX smoke and Provider target must explicitly close
   `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL`; remove stale generated build-copy
   trees before installation.
4. Build one complete SIF only with
   `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`.
   Compile `_ndnsf.so`, Provider binaries, and all container-bound native code
   inside the SIF build stage or a sealed ABI-identical builder rootfs.
5. In that exact SIF, verify the final Provider/extension census and hashes,
   Python import, real runner check, RPATH, and `ldd` closure. Run the
   `ndnsf-sif-library-lock-v2` checker over every packaged `.so`; reject stale
   base artifacts, host RPATHs, unresolved SONAMEs, duplicate libraries, or
   SOABI/EXT_SUFFIX mismatches.
6. Re-run full C++ unit/integration suites, the Python contract suite with the
   repository `PYTHONPATH`, and bounded MiniNDN request → ACK_CLOSED →
   Selection → Response plus negative authorization/replay/tamper/partial
   cases. READY, import, or GPU visibility alone is not a service PASS.
   For the adapter-specific CPU gate, generate the deterministic fixture with
   `tests/fixtures/spec170/generate_cpu_onnx_fixture.py`, set
   `NDNSF_DI_TEST_ONNX_MODEL`, and run the focused
   `DistributedInferenceCollectiveRuntime` group. An unset fixture is a skip,
   not evidence; the enabled run must check both real ONNX outputs and
   `ExecutionEvidence` for both ranks.
7. Write the build record, SIF hash, closure report, source seal, bundle hashes,
   and test outputs under the new evidence root. Copy exactly one SIF to
   project storage, verify its remote hash, and repeat the closure check on the
   staged bytes.
8. Run one bounded Tiger verification. Stage the SIF once to job-local
   scratch, verify staged hash and Apptainer version, `cd "$BUNDLE"` before
   every Provider launch, check all relative artifacts, and require controller,
   every Provider, and User exit status plus the complete request lifecycle.

### No-repeat preflight (must be recorded before a build)

Run these checks in order and save their outputs under the new candidate
evidence directory. A failure stops the update; it is not a reason to spend a
new SIF or Tiger allocation.

```text
1. Disk: record `df -h` and enumerate exact disposable paths; keep one active
   SIF, one source seal, one build record, and canonical evidence.
2. Identity: create a new candidate ID and bind source, lock, definition,
   bundle, and workload to that ID; an existing r23 image is allowed only
   when its recorded source seal and native hashes match exactly.
3. Toolchain: compare the explicit local Apptainer binary
   `/opt/apptainer/1.5.3/bin/apptainer` with the allocated target node; require
   the current 1.5.3 pair (packaging suffixes may differ). Record both
   `command -v apptainer` and the binary actually invoked so a stale PATH
   entry cannot silently change the build.
4. Boundary: run the source/build-boundary validators; host Python headers,
   venvs, Linuxbrew native libraries, and host `.so` files are not inputs.
5. Target census: enumerate every Provider, ONNX, and Python-native target;
   require each target's own `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` closure.
6. Build: invoke only `build-local-sif.sh` and require a complete application
   SIF plus a checksum-bound record; never build Docker/OCI or on Tiger.
7. Exact-SIF gates: inside that SIF verify the source seal, one active
   `_ndnsf.so`, Provider/framework hashes, Python import, real runner,
   RPATH/ldd, and `ndnsf-sif-library-lock-v2` for every packaged `.so`. The
   gate driver must select `/opt/apptainer/1.5.3/bin/apptainer` explicitly
   (or an explicit `SPEC170_APPTAINER` override) and verify version 1.5.3; a
   bare PATH lookup is not acceptable.
8. Functional gates: run full C++/Python tests, deterministic real CPU ONNX,
   MiniNDN/in-process Request → ACK_CLOSED → Selection → Response, and the
   negative authorization/replay/tamper/partial cases.
9. Promotion: copy exactly one SIF, verify local/remote/staged hashes, then
   let Tiger verify and execute only that immutable file.
```

The first two gates are independent: a SIF may be physically readable and pass
its own import/`ldd` checks while still being invalid for the current checkout
because its source seal or native hashes are stale. Conversely, a source match
without an internal closure or lifecycle PASS is not a usable release.

### Required update record

Keep one small machine-readable record beside every candidate. It must state
the candidate ID, source/lock/definition and bundle digests, local and target
Apptainer versions, build script, SIF size and SHA-256, `containerNativeBuild`,
`hostBinaryInputs`, Python ABI (`SOABI`/`EXT_SUFFIX`), Provider and
`_ndnsf.so` hashes, library-closure result, exact test commands/results, and
the Tiger action (`verify-hash-and-execute-only`). Also record any failed
predecessor and why it was rejected. A SIF is “usable” only for the exact
source and lock identity recorded in this file; source changes after the
build invalidate the SIF even when its old import or Tiger smoke passed.

The current recurring incidents are recorded explicitly rather than inferred
from a successful smoke: host-built Python extensions (`WRONG_BUILD_BOUNDARY`),
missing dependency closure on sibling Waf targets, stale extensions in a base
SIF, relative artifacts without `cd "$BUNDLE"`, shared HOME/PIB locks,
controller/Provider/User startup races, a stopped Face event loop during
large-data fetches, and disk exhaustion from duplicate SIF/model copies.
Run the checklist before every new SIF; do not repair any of these in a live
Tiger allocation.

### Recurring hard stops

| Symptom | Required action |
|---|---|
| `Python.h`, `cpXY`, or host `.so` problem | `WRONG_BUILD_BOUNDARY`; rebuild inside the sealed SIF ABI, never alter host Python. |
| One Waf target passes while a sibling target fails | Fix the complete target census and rebuild all targets. |
| Import/`ldd` passes but a base SIF contains an old extension | Remove old Provider/framework/`_ndnsf*.so`; create a new candidate. |
| Relative model/artifact is missing | Provider launch must `cd "$BUNDLE"`; rerun bundle preflight. |
| READY but no request response, `allowed=[]`, PIB lock, or DKEY timeout | Fix isolated HOME/PIB, certificates, bootstrap ordering, and lifecycle evidence; do not raise timeouts blindly. |
| Valid SIF but wrapper fails after promotion | Preserve the SIF and classify orchestration failure separately; do not rebuild the image. |
| Host/Tiger Apptainer, Boost, ONNX Runtime, or ABI differs | Reject before staging; align toolchains and create a new identity. |
| Disk falls during build/staging | Stop, preserve evidence, clean only enumerated paths, verify free space, then retry with a new identity. |
| Linuxbrew/host `ld` pulls unrelated GTK/UAV/system `.so` files into a link | Classify `HOST_TOOLCHAIN_CONTAMINATION`; isolate the sealed builder and rerun the complete target census and internal closure gate. |
| SIF imports but its source seal or native hashes differ from the checkout | Reject the candidate as stale; create a new source-bound SIF identity. |
| Model config is not recognized by the image lock | Stop before MiniNDN; preserve the compatibility failure and build a new sealed runtime/SIF. Do not repair with host pip or a temporary overlay. |
| A selected integration test passes while the real request path is untested | Do not call the candidate PASS; run the exact-SIF functional lifecycle and negative gates. |

At minimum, retain `source-seal.json`, `build-record.json`, `runtime.sif` and
SHA-256, `container-native-build.json`, `sif-library-closure.json`, bundle
preflight, local test reports, Tiger command/staged-hash records, and negative
evidence for every failed predecessor. The Spec170 ledger remains authoritative
for formal completion; this contract prevents process drift.

Use this document as the normal entry point for NDNSF-DI experiments. The
canonical source of runtime configuration is the runtime profile; avoid calling
individual experiment scripts directly unless you are debugging one script.

For the Spec 105 systemd/operator surface, use
[`NDNSF-DI-deployment-candidate.md`](NDNSF-DI-deployment-candidate.md) and the
packaging runbook. Production `provider|run|bench` execute explicit no-shell
adapters; the old simulated run/bench/context sweep lives only under
`ndnsf-di contract-smoke`. Doctor/status/metrics require digest-bound identity
and fresh release/plan/evidence telemetry. The current candidate is BLOCK and
Spec 106 remains physical-only.

## Canonical Profile

The default DI profile is:

```bash
examples/di-native-tracer.runtime.json
```

It records the NativeTracer harness, topology, Qwen tiny proportional model
artifacts, 2GB/4GB/8GB provider profiles, runtime knobs, token settings,
requests, concurrency, target RPS, and timeouts.

## Normal Flow

1. Validate the profile before a long run:

```bash
python3 tools/ndnsf_runtime.py di validate
```

2. Print the resolved profile when you need to inspect absolute paths and
   defaults:

```bash
python3 tools/ndnsf_runtime.py di print
```

3. Run the DI doctor and save the resolved configuration:

```bash
python3 tools/ndnsf_runtime.py di doctor \
  --event-log /tmp/ndnsf-di-runtime-events.jsonl \
  --write-resolved /tmp/ndnsf-di-runtime-resolved.json
```

4. Dry-run the experiment command before spending MiniNDN time:

```bash
python3 tools/ndnsf_runtime.py di run --dry-run -- --out /tmp/ndnsf-di-run
```

5. Run the experiment from the saved resolved profile:

```bash
python3 tools/ndnsf_runtime.py di run \
  --resolved /tmp/ndnsf-di-runtime-resolved.json \
  -- --out /tmp/ndnsf-di-run
```

Arguments before `--` belong to the wrapper. Arguments after `--` are passed to
the underlying experiment script and override profile defaults.

For a runtime-aware smoke run that starts MiniNDN but does not run the full
user/provider request path:

```bash
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --runtime-profile examples/di-native-tracer.runtime.json \
  --out /tmp/ndnsf-spec047-minindn-smoke \
  --requests 2 \
  --concurrency 1 \
  --provider-check-timeout 45 \
  --no-local-execution-only
```

The default profile keeps `local_execution_only=true` so routine checks stay
fast. Use `--no-local-execution-only` when you intentionally want MiniNDN.

For the short full-network evidence path that exercises controller,
providers, user driver, ACK/Selection/Response, and dependency exchange:

```bash
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --runtime-profile examples/di-native-tracer.runtime.json \
  --out /tmp/ndnsf-spec047-full-network-audit \
  --requests 2 \
  --concurrency 1 \
  --provider-check-timeout 60 \
  --no-local-execution-only \
  --full-network \
  --tracer-deterministic-runner
```

## Common Commands

Tk operator console:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 Experiments/NDNSF_DI_GUI.py
```

The GUI's first three tabs are `User`, `Provider`, and `Controller`. Loading a
profile does not start anything. A role starts only when its tab's Run button
is clicked, and all three roles may run at the same time. The User tab includes
a request/response panel for normal service requests and collaboration request
JSON inputs. The default reusable profile is:

```bash
examples/python/NDNSF-DistributedInference/gui_three_role_profile.json
```

The GUI is an operator entrypoint. CLI commands below remain the reproducible
evidence path for MiniNDN campaigns and paper-quality measurements.

Headless GUI automation uses the same profile and runtime controller as the Tk
tabs, but does not create a display window. Use fake mode for CI-style logic
checks:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 Experiments/NDNSF_DI_GUI.py \
    -headless \
    -controller_auto_run \
    -provider_auto_run \
    -user_auto_run \
    -user_config=examples/python/NDNSF-DistributedInference/gui_user_hello.config \
    -provider_config=examples/python/NDNSF-DistributedInference/gui_provider_hello.config \
    -controller_config=examples/python/NDNSF-DistributedInference/gui_controller_hello.config \
    --runtime-mode fake \
    --send-user-request \
    --output-json /tmp/ndnsf-di-gui-headless.json
```

Use `--runtime-mode direct` only inside a prepared NFD or MiniNDN environment,
because it constructs the real `ServiceController`, `ServiceProvider`, and
`ServiceUser` wrapper objects. The `-user_config`, `-provider_config`, and
`-controller_config` files are JSON role overrides; they may also be wrapped
under a top-level `user`, `provider`, or `controller` key.

MiniNDN GUI preflight without opening the window:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 Experiments/NDNSF_DI_GUI_Minindn.py --preflight-only
```

This preflight verifies imports, policy loading, and the headless fake
Controller/Provider/User request path. Add `--run-minindn --case yolo-2x2` when
you also want the existing MiniNDN regression before launching or skipping the
GUI.

GUI headless Qwen NativeTracer MiniNDN experiment:

```bash
sudo -n PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache \
  python3 Experiments/NDNSF_DI_GUI.py \
    -headless \
    --headless-experiment qwen-minindn \
    --experiment-runtime-profile examples/di-native-tracer.runtime.json \
    --experiment-out /tmp/ndnsf-di-gui-qwen-headless-minindn \
    --experiment-requests 1 \
    --experiment-concurrency 1 \
    --experiment-provider-check-timeout 60 \
    --output-json /tmp/ndnsf-di-gui-qwen-headless-minindn/gui-headless-summary.json
```

This entrypoint keeps the GUI profile as the operator-facing configuration
surface, but delegates the network run to the canonical
`NDNSF_DI_NativeTracer_Minindn.py` harness. It forces the Qwen proportional
planner path with `--assignment llm-proportional`, `--policy-bundle
llm-proportional`, `--llm-planner-mode proportional`, `--full-network`, and
`--no-local-execution-only`. Use `--experiment-dry-run` first when checking a
configuration without starting MiniNDN.

The non-headless GUI exposes the same path in the `Qwen MiniNDN` tab. Edit the
runtime profile, output directory, request count, concurrency, provider-check
timeout, target RPS, open-loop duration, and extra harness arguments there,
then click `Preview Command` or `Run Qwen MiniNDN`. The tab uses the same
`build_qwen_minindn_command()` helper as headless mode; the only difference is
that the GUI lets an operator edit fields and click Run instead of passing
`--headless-experiment qwen-minindn`. Keep `Wrap with sudo -n env` enabled when
starting MiniNDN from a normal desktop session.

After a run, the tab displays the decoded core envelope summary from
`summary.json`: provider readiness, reason codes, service-payload schemas,
operation states, latest provider queue/active-work values, and the legacy ACK
runtime hint counters. Use `Refresh Summary` to reload these fields from the
current output directory without rerunning MiniNDN.

For a small GUI-driven campaign, set `Target RPS sweep list` to a comma-separated
list such as `0.2,0.4,0.8`, set `Sweep repeats`, then click `Run Sweep`. The GUI
runs the same Qwen MiniNDN command sequentially for each RPS/repeat and writes
each run under a separate subdirectory such as `rps-0_4-run-1`. Use `Dry run
only` first to verify the expanded command list without starting MiniNDN. When
`Output JSON` is set, the GUI also writes sibling CSV, Markdown, and SVG
reports. The CSV includes status, runner mode, target RPS,
request/success/failure counts, p50/p95/mean/makespan latency, throughput,
dependency status, dependency roles, provider count, mean provider utilization,
and total provider busy handler time. The Markdown report summarizes the run
count, failed runs, best p50, best throughput, provider utilization, and the
per-run `summary.json` paths. It also embeds the SVG plot, which shows latency,
throughput, and provider utilization for the sweep runs.

GUI tests are layered:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_tk_gui.py

xvfb-run -a env PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_tk_widgets.py

xvfb-run -a env PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_gui_visual_smoke.py
```

The first command tests the non-display headless/runtime helpers. The second
creates a real Tk window under Xvfb and checks the `User`, `Provider`, and
`Controller` tabs, editable fields, Run buttons, and User request panel through
the same fake runtime factory. The third is optional and only captures a real
window screenshot when PyAutoGUI is installed; it is not the source of truth for
NDNSF-DI behavior.

Single NativeTracer harness run:

```bash
python3 tools/ndnsf_runtime.py di run -- --out /tmp/ndnsf-di-run
```

LLM full-network campaign:

```bash
python3 tools/ndnsf_runtime.py di campaign -- --runs 1 --workloads c1:1:1
```

NativeTracer rate sweep:

```bash
python3 tools/ndnsf_runtime.py di sweep -- --target-rps-list 0,1,2
```

For measured open-loop work, use the NativeTracer harness directly with a
60-second window, a request cap of `ceil(rate * 60)`, and the `threaded` driver.
Apply the Spec 093 scheduling, completion, throughput, dependency, and malformed
trace gates to every point. The former runtime-aware sweep was removed because
its deterministic runner and success-only stability check could label an
invalid offered-load point as stable.

Planner-only LLM proportional RPS search:

```bash
python3 tools/ndnsf_runtime.py di search -- --target-rps-list 1,5,10
```

Use `--dry-run` with `di run`, `di campaign`, `di sweep`, or `di search` when
you only want to inspect the generated command.

## What Each Step Catches

- `di validate`: misspelled keys, wrong scalar types, unsupported enum values,
  and missing DI sections.
- `di print`: the effective profile after default resolution.
- `di doctor`: missing artifacts, missing topology, missing binaries, NFD socket
  status, and the ready-to-run MiniNDN command.
- `di run`: one NativeTracer execution path.
- `di campaign`: repeated full-network LLM workload runs.
- `di sweep`: NativeTracer request-rate sweep.
- `di search`: planner-side greedy versus proportional RPS search.

## Runtime-Aware User-Side Planner Boundary

Spec047 keeps planning in the user process, but separates reusable NDNSF core
metadata from DI-specific inference semantics.

NDNSF core metadata is service-neutral:

- `GenericAckMetadata`: an ACK envelope for structured provider state.
- `GenericProviderRuntimeHint`: queue length, active work, wait estimate,
  capacity hints, confidence, and directed peer metrics.
- `PeerNetworkMetric`: directed RTT, bandwidth, loss, jitter, staleness, and
  confidence for provider-to-provider edges.
- `GenericAdmissionLease`: an optional, short-lived admission/resource proof.

NDNSF-DI interprets the service payload:

- `ModelFragmentKey`: digest-based model/split/stage/shard identity.
- `DiFragmentRuntimeState`: GPU, CPU, disk, repo, or missing residency state.
- `DiLeaseResourceBinding`: DI role plus fragment binding inside a generic
  lease.
- `ProviderNetworkMatrix`: graph-placement edge-cost view over directed peer
  metrics.

Runtime-aware NativeTracer planning can consume a matrix from an earlier probe
or run:

```bash
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --runtime-profile examples/di-native-tracer.runtime.json \
  --runtime-aware-user-planner \
  --provider-network-matrix-json /path/to/previous-summary.json \
  --out /tmp/ndnsf-di-network-aware-run
```

The input may be a raw `ProviderNetworkMatrix` JSON file or a previous
NativeTracer `summary.json` containing `providerPairTelemetry.matrix`.
Telemetry from the current run is written after execution, so it is evidence
for the next planning run or campaign phase, not retroactive input to the plan
that already started.

Full-network NativeTracer runs collect this evidence by default with a
dependency-edge ndnping probe after provider provisioning and before the user
workload. The resulting file is:

```text
<run-out>/dependency-edge-ndnping-rtt-stats.json
```

Use `--skip-provider-pair-telemetry-probe` only for fast smoke runs where
provider-pair telemetry is not needed.

Admission leases are opt-in. Existing non-lease services keep the current
ACK/Selection/Response path and still rely on ProviderToken, UserToken,
NAC-ABE, provider permissions, and replay protection. A lease is only an
admission-control proof; it is not a replacement for those security checks.

For multi-user contention, each user plans from current typed ACK/runtime hints
and then acquires provider-owned execution leases. Rejection is explicit and
bounded; the user replans from fresh provider state. There is no DI coordinator
process, cross-user assignment authority, or generic Core coordination
envelope. The earlier advisory path failed its matched retention experiment and
was removed. Provider-owned admission remains the single execution authority.

## Runtime-Aware Campaign Outputs

Spec047 runs now write these stable files in the result directory:

```text
summary.json
summary.txt
planner-metrics.json
planner-metrics.csv
assignment.csv
runtime-v1/runtime-v1-minindn-evidence-summary.json
```

`planner-metrics.json` is the compact surface for paper or slide evidence. It
records p50/p95/mean latency when the user path runs, success rate, provider
utilization, lease counters, planner-selected residency counters, observed
provider residency counters, edge-cost summary, and bounded replan count. In
local-execution-only and provider-check runs, user latency is zero because the
full user request path is intentionally gated; the local plan, manifest,
runtime-v1 evidence, and MiniNDN provider placement are still validated.

`summary.json` is the broader run record. For core/app-boundary checks, read
`coreEnvelopeSummary`: it decodes provider ACK payloads that carry typed
`ProviderCapabilityHint` and nested `ServiceOperationStatus` envelopes. This
section reports provider readiness, negative/admission reason codes,
service-payload schemas, operation states, and each provider's latest core
runtime view. The older `providerAckRuntimeHints` section remains for legacy
queue/worker fields.

For execution identity, read `executionEvidence` and the mechanically derived
`runnerClassification`. Each evidence record is emitted by an initialized
provider backend and binds provider boot/epoch, roles, model and artifact
digests, plan digest, runtime version, and device. `runnerMode` is a deprecated
derived compatibility field; it is never a real-compute truth source. Inspect a
run without relying on legacy labels with:

```bash
python3 tools/ndnsf_runtime.py di evidence --summary /path/to/summary.json
```

Missing, mixed, contradictory, synthetic, or digest-mismatched evidence blocks
the Spec 105 MiniNDN candidate release gate.

For Qwen NativeTracer MiniNDN runs, the C++ native provider emits both forms:
legacy semicolon ACK fields for old parsers and
`providerCapabilityHint=json64:<json>` for the core envelope summary. A healthy
small run should show `coreEnvelopeSummary.envelopeCounts.providerCapabilityHint`
greater than zero and `providerReadiness.ready` matching the observed ACK
events.

Provider fragment residency should come from `ProviderFragmentInventoryManager`
when the provider runtime can expose local state. The manager treats GPU and CPU
residency as explicit runtime load/evict events, treats disk residency as the
presence of the configured local artifact file, and falls back to
`REPO_AVAILABLE` or `MISSING` when the fragment is not local. The resulting
`DiProviderRuntimeState` is embedded in `GenericAckMetadata.servicePayload`, so
the user-side planner can prefer already-loaded or already-resident fragments
without putting model-specific concepts into NDNSF core.

The native C++ provider also emits provider-local inventory events:

```text
NDNSF_DI_FRAGMENT_INVENTORY event=CPU_RESIDENT provider=/P role=/Backbone \
  fragmentDigest=sha256:... backend=onnx-cpu path=/tmp/stage.onnx \
  residency=CPU_RESIDENT epoch_ms=...
```

`DISK_RESIDENT` is emitted before runner creation, `CPU_RESIDENT` or
`GPU_LOADED` after runner creation depending on the runtime/device metadata,
`EXECUTION_OBSERVED` when the provider actually executes that role, and
`EVICTED` when the provider runtime is released. Current ONNX CPU runs should
normally report CPU residency, not GPU residency. The MiniNDN harness scans
these events into `providerFragmentInventory`, including `eventCounters`,
`residencyCounters`, `latestByProviderRole`, and `latestByFragment`.
`planner-metrics.json.residencyCounters` remains the planner-selected residency
view; `planner-metrics.json.observedResidencyCounters` is the provider-log
observation view.

For multi-user evidence, report both direct lease counters and residency hits:

```text
leaseCounters.granted / rejected / expired / consumed
residencyCounters.GPU_LOADED / CPU_RESIDENT / DISK_RESIDENT / REPO_AVAILABLE / MISSING
observedResidencyCounters.CPU_RESIDENT / GPU_LOADED / DISK_RESIDENT
latencyMs.p50 / latencyMs.p95
maxStableRps from the RPS sweep
```

This is the minimum evidence needed to show whether user-side plans are being
controlled by provider admission leases and whether provider-local model
fragments are actually being reused.

The campaign harness scans provider logs for NativeTracer lease grants plus
`NDNSF_ADMISSION_LEASE_ACCEPTED` and `NDNSF_ADMISSION_LEASE_REJECTED`.
NativeTracer full-network sweeps now enable generic admission leases by default:
providers grant one lease in each successful readiness ACK, the Python
collaboration selector copies `leaseId` and `resourceBindingProof` into each
provider assignment payload, and providers consume the lease before executing
the selected role. Use `--disable-native-admission-lease` on the RPS sweep
wrapper only for an explicit no-lease comparison.

Expected healthy lease-enabled evidence for a two-request four-role smoke is
`granted > consumed`, `consumed = 8`, `rejected = 0`, and
`providerFragmentInventory.eventCounters.EXECUTION_OBSERVED = 8`.

Use two RPS modes carefully:

- Closed-loop sweeps with `--requests` and `--concurrency` validate end-to-end
  correctness, lease counters, residency counters, and latency, but
  `--target-rps` is only planner/load evidence unless `--open-loop-duration-s`
  is set. A 2026-07-05 lease/no-lease comparison at target RPS
  `0.2,0.4,0.8,1.2` kept observed throughput near `0.203 RPS` for every
  point, so it did not create a real high-load conflict.
- Open-loop sweeps with `--open-loop-duration-s` do schedule by target rate.
  The same 2026-07-05 comparison with a 20-second window succeeded at `0.2`
  RPS, but `0.4`, `0.8`, and `1.2` failed with
  `local-open-loop-backpressure` in both lease-enabled and no-lease runs. That
  means the current user-side child-process driver hits local backpressure
  before provider admission leases become the bottleneck.

For a defensible high-concurrency lease result, first use or implement a user
driver that can keep the offered load close to the target rate without local
backpressure, then rerun the same lease/no-lease comparison.

Spec 091 screened the three existing drivers at 1 RPS for 60 seconds with
concurrency 4. `child` completed 60/60 but achieved only 0.410 RPS with 77.96 s
maximum schedule slip. `threaded` failed before a complete workload summary
because worker users could not retrieve scope-key large Data while the base
publisher was not kept running. The removed process-pool experiment completed
60/60 with no local
backpressure and all dependency events, but its reported 0.932 RPS includes an
intentional five-second schedule lead and lacks per-worker slip telemetry.
Therefore this is a user-driver/instrumentation boundary, not provider-capacity
evidence. Do not quote a maximum stable RPS from this screening; see
`specs/091-native-di-offered-load-baseline/evidence/`.

Spec 092 fixed the base scope-key publisher lifecycle and open-loop timing
instrumentation. At the same 1 RPS, 60-second, concurrency-4 Qwen point, the
threaded driver passed three matched repetitions: 180/180 requests, 720/720
dependency events, mean 1.0133 RPS, mean p50 211.8 ms, mean p95 1176.4 ms, and
worst maximum schedule slip 14.8 ms. Use `threaded` for the next offered-load
search. The removed process-pool experiment completed 60/60 at 1.012 RPS but
failed the scheduling gate with 4345.5 ms startup slip. These results do not establish a
maximum stable RPS; see `specs/092-native-di-user-driver-correctness/evidence/`.

Spec 093 extends the same threaded Qwen fixture through 2, 4, and 8 offered
RPS. All points pass the scheduling and system gates. At 8 RPS, three matched
runtime treatments complete 1440/1440 requests with mean 7.9850 RPS, mean p50
198.5 ms, mean p95 247.6 ms, and worst schedule slip 16.24 ms. Dependency trace
markers total 5760; one line is explicitly retained as an observability parse
error caused by concurrent plain-text logging. The busiest stages reach about
26% estimated utilization, so no limiting layer is reached within 1-8 RPS.
State this as **stable through the highest tested point of 8 RPS**, never as a
maximum stable RPS. See `specs/093-native-di-threaded-rps-boundary/evidence/`.

The multi-user fixture is:

```bash
examples/python/NDNSF-DistributedInference/native_di_tracer/runtime_aware_fixtures/multi_user_requests.json
```

The directed provider-to-provider metric fixture is:

```bash
Experiments/Topology/AI_Lab_RuntimeAwarePeerMetrics.json
```

It is separate from `AI_Lab.conf` because MiniNDN topology links are symmetric,
while runtime-aware DI planning needs directed overlay metrics such as
provider A to provider B RTT/bandwidth versus provider B to provider A.

## When To Use Lower-Level Scripts

Use the wrapper first. Drop down to lower-level scripts only when you need to
debug script-specific behavior. The lower-level scripts still accept:

```bash
--runtime-profile examples/di-native-tracer.runtime.json
--runtime-resolved /tmp/ndnsf-di-runtime-resolved.json
```

Command-line flags on those scripts override profile defaults.

## Result Hygiene

For each meaningful run, keep the result directory and the summary files it
produces, such as JSON, CSV, lifecycle traces, or campaign summaries. If a run
is only a failed smoke or local troubleshooting attempt, delete it after the
useful finding is documented.
