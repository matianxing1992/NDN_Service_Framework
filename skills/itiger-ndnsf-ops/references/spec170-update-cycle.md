# Spec170 NDNSF-DI update cycle

Start with the repository's concise operator handoff:
[`spec170-update-lessons-20260819.md`](https://github.com/matianxing1992/NDN_Service_Framework/blob/main/specs/170-reusable-layer-artifacts/evidence/spec170-update-lessons-20260819.md).
It is the current snapshot of the usable SIF, fixed configuration, and
recurring failure classes. This reference supplies the executable gates; the
handoff prevents rediscovering the same build-boundary and storage mistakes.

Use this reference whenever NDNSF-DI source, dependencies, native bindings, or
the Apptainer definition changes. The repository version is
`docs/NDNSF-DI-runtime-workflow.md` (the "Source-to-SIF update contract"
section); this file is the skill's short operator memory aid.

## One-way update rule

```text
candidate-input inventory
  -> source/lock/definition/native runtime change
  -> new candidate ID -> local Apptainer build
  -> exact-SIF closure -> local functional gates
  -> hash-bound Tiger execution
```

Never mutate or repair an existing SIF in place. A workload,
policy, route, prompt, or externally mounted model change may reuse the SIF,
but requires a new bundle/manifest identity and bundle preflight.

## Existing-SIF decision rule

Treat “usable SIF” as a two-part claim:

1. the exact image passes its sealed-source/runtime checks; and
2. its source seal, dependency lock, definition, Provider/framework/
   `_ndnsf.so` hashes, and Python ABI match the source about to be tested.

If the source or native/runtime inputs changed, the old image may remain as a
control, but it is stale for the new run. Create a new candidate ID and build
one complete local SIF. Never copy a new extension or Provider into an old SIF,
bind a host virtual environment, or call an old import/Tiger smoke a current
source PASS. If only an externally mounted workload, policy, route, prompt, or
model changes, keep the SIF and create a new hash-bound bundle/manifest instead.

## Current sealed candidate (2026-08-19)

The currently usable local image is:

```text
SIF:            .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SIF SHA-256:    5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
bytes:          4442767360
record:         .codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
sourceRevision: 989a9daace669a4f93496dade3176c527edb2469
sourceSeal:     sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e
Apptainer:      `/opt/apptainer/1.5.3/bin/apptainer` 1.5.3; target record 1.5.3
Python:         /opt/venv/bin/python 3.10.18
SOABI:          cpython-310-x86_64-linux-gnu
containerNativeBuild=true; hostBinaryInputs=[]
tigerAction=verify-hash-and-execute-only
```

Exact-image probes passed: `import ndnsf._ndnsf`, exactly one active
`_ndnsf*.so`, zero unresolved extension `ldd` dependencies, and the real
Provider `--check-only --wiring-check-only` plan check on the read-only r23
D2b bundle. The full probe record is
`specs/170-reusable-layer-artifacts/evidence/current-sif-r23-local-20260819.md`.
These probes establish image usability for the recorded source only; they do
not establish T029/T036 completion, current dirty-tree parity, GPU acceptance,
or Tiger promotion.

The same-file reuse gate was rerun after the 2026-08-19 cache cleanup:
`sha256sum` matched the record, the metadata-only build-record validator
returned `status=PASS`, and an exact-SIF Python import printed
`NDNSF_SIF_IMPORT_OK`. The filesystem had 75 GiB available at that check.
The durable evidence is
`specs/170-reusable-layer-artifacts/evidence/current-sif-reuse-gate-20260819.md`.
This is a physical-usability result for r23, not permission to run the dirty
working tree; any source/native/lock/definition change still creates a new
candidate and complete local SIF.

One lifecycle-evidence warning remains part of the current baseline: a
combined D0→D1 run once emitted an empty/zero-byte producer timing record for
`backbone-to-head0`, although the consumer fetched 120 bytes and the final
response succeeded. Keep the analyzer strict; reproduce and fix the
producer-side timing/instrumentation race in the next source-bound candidate
instead of weakening the evidence rule.

At the current checkpoint, the working tree is still not source-sealed: its
controlled diagnostics are recorded separately, and the auxiliary
authorization/security run is 77/78 because eleven registered paths differ
from the older frozen inventory. Treat that as a source-integrity blocker;
do not edit the inventory to force a green result. Select and review the
intended source, regenerate its seal/inventory, and only then start the next
local SIF candidate. Until the remaining lifecycle/negative gates and T029
freeze close, r23 is bounded verification, not a release.

The working tree may contain uncommitted source or generated files. Before
reusing r23, compare the candidate's source seal and native hashes with the
source intended for the run. Any mismatch creates a new candidate identity and
new local SIF; never patch r23 in place.

### Cheap reuse gate

Run this before a new allocation, upload, or experiment. Hash the multi-GB SIF
once, validate the record without rereading it, and perform the import inside
the exact image:

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

This is an identity/usability gate, not a current-source or protocol PASS.
Compare the recorded source seal and native hashes with the checkout. A
source, dependency, definition, lock, NDN-SVS/NDN-CXX, C++, or Python change
requires a new candidate and local SIF; never copy a host extension or patch
the image in place. Keep one active image and enumerate exact disposable paths
before removing any predecessor.

## Required checks before each build

1. Check disk and enumerate exact disposable paths; do not use broad cleanup.
2. Generate `candidate-input-inventory.json` with
   `tools/ndnsf-di/collect_spec170_candidate_inputs.py`; retain its digest and
   compare it with the sealed candidate before promotion. This is a pre-freeze
   coverage check, not T029's frozen manifest.
3. Match Apptainer with the target compute-node allocation; login-node output is
   not authority. Invoke `/opt/apptainer/1.5.3/bin/apptainer` explicitly and
   record both that path and `command -v apptainer`; an ambient older PATH
   binary must not silently drive the build.
4. Run source-seal/build-boundary checks and the complete Waf target census.
5. Build only through `build-local-sif.sh`; no Docker/OCI/Tiger-side build.
6. Compile `_ndnsf.so` and all native code inside the SIF ABI or sealed builder.

7. For every new model revision, compare its `model_type`, declared
   Transformers version, tokenizer and file hashes with the exact image lock;
   run the architecture-specific import and a standalone same-model
   load/generate probe inside the candidate SIF before MiniNDN. If the import
   or load fails, stop and create a new candidate after correcting the sealed
   runtime. A host package install or temporary overlay is diagnostic evidence,
   never a release fix.

The current tested configuration is the explicit local binary
`/opt/apptainer/1.5.3/bin/apptainer` and compute Apptainer `1.5.3`, the
`build-local-sif.sh` entry point, `/opt/venv/bin/python` with Python-3.10
site-packages in the image, and explicit
`BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` closure on every Provider and ONNX
target. Record free space on every run (75 GiB was free after the 2026-08-19
cache cleanup; 53 GiB was the pre-cleanup observation). Keep one active SIF and avoid creating duplicate
multi-gigabyte images until named disposable paths have
been enumerated and their evidence retained.

The current Qwen3 check is a concrete recurrence shield: the pinned
`Qwen/Qwen3-0.6B` snapshot is cached once, while r23's Transformers 4.48.2 and
the host child 4.46.3 do not recognize `model_type=qwen3`. A temporary
Transformers 4.51.3 overlay loaded the model only as a diagnostic and must not
be mounted into a promoted SIF. The detailed result is
`spec170-qwen3-toolchain-preflight-20260819.md`; the repair is a new sealed
Qwen3-compatible runtime/SIF, not host pip changes or a duplicate model copy.

The host linker is not part of the runtime ABI. Linuxbrew/host `ld` resolving
unrelated GTK/UAV/system libraries is `HOST_TOOLCHAIN_CONTAMINATION`, not a
successful full-build result. Do not copy the resulting host binary into the
SIF; rebuild the complete target set in the SIF builder and rerun the internal
library-lock gate.

### Host MiniNDN diagnostic rebuild boundary

After a C++ framework or Python binding change, a host MiniNDN run may still
load an old shared library or `_ndnsf.so`. Refresh that diagnostic stack before
interpreting its result:

```text
./waf build --targets=ndn-service-framework -jN
(cd pythonWrapper && python3 setup.py build_ext --inplace --force)
verify extension/framework hashes and ldd
run focused C++/Python tests, then the real MiniNDN lifecycle/negative gate
```

This host-built extension is diagnostic-only and must never enter a SIF. The
candidate image follows the separate sealed boundary: compile its framework
and `_ndnsf.so` in the SIF builder, then run the exact-SIF import, ABI,
RPATH/`ldd`, library-lock, and lifecycle gates inside that image. The latest
diagnostic evidence illustrates why this is explicit: two real CPU MiniNDN
positive hybrid cases passed after the refresh, but the cancellation negative
gate remained open (`LEASE_CAPACITY_REJECTED` at the default 5 s lease timeout;
`CANCEL_REJECTED` after activation with a 20 s diagnostic timeout).

## Required checks before promotion

- final in-SIF Provider/extension census and hash match;
- Python import and real runner check inside the exact SIF;
- `ldd`/RPATH and `ndnsf-sif-library-lock-v2` closure for every packaged `.so`;
- full local unit/integration/Python plus MiniNDN request lifecycle and
  negative-auth tests;
- one source/lock/SIF/bundle/record identity with no stale base artifact;
- remote and staged SIF SHA-256 equality.

## Recurrence shields

`Python.h` or `cpXY` drift is `WRONG_BUILD_BOUNDARY`; do not install host
headers. A passing sibling Waf target does not qualify missing `NDN_CXX`,
`NDN_SVS`, Boost, ONNX Runtime, or `DL` on another target. A clean import does
not excuse a stale extension copied from a base SIF. A READY marker does not
prove Request → ACK_CLOSED → Selection → Response. Relative artifacts require
both generated-bundle materialization (file plus `.sha256` sidecar) and
`cd "$BUNDLE"`. Isolated HOME/PIB and the controller→Provider bootstrap→User
barrier are mandatory for NAC-ABE. Provider launch subshells must use
`exec env ... di-native-provider` so cleanup tracks the executable PID; a
post-run process census must find no job-local Provider. Preserve every failed
candidate and use a new identity after repair.

The prior bounded-verification candidate is documented in
`specs/170-reusable-layer-artifacts/evidence/README.md`; it is not a T029-frozen
release. Keep the active SIF, record, hashes, and canonical evidence while
cleaning only named superseded artifacts.

## Mandatory no-repeat preflight

Before starting a new local SIF build, write a candidate evidence directory and
record each result below. Any failed row is a hard stop.

| Order | Gate | Required evidence |
|---:|---|---|
| 1 | Disk and retention | `df -h`, exact disposable paths, one active SIF, no duplicate model/image copy. |
| 2 | Candidate identity | New source/lock/definition/bundle/workload ID; no stale `/tmp` or old-SIF selection. |
| 3 | Apptainer parity | Allocated target-node version and local executable/hash; current pair is 1.5.3. |
| 4 | Build boundary | Source seal and `spec170_sif_build_boundary.py` PASS; no host Python headers, venv, `.so`, or Linuxbrew runtime input. |
| 5 | Complete target census | Every Provider/ONNX target declares `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL`; build all siblings. |
| 6 | Local SIF | `build-local-sif.sh` emits one complete `runtime.sif` and record; no Docker/OCI/Tiger materialization. |
| 7 | Exact-SIF closure | In-SIF import, real runner, one active extension census, Provider/framework hashes, RPATH/`ldd`, and `ndnsf-sif-library-lock-v2` PASS for every packaged `.so`. |
| 8 | Functional regression | Full C++/Python, real CPU ONNX, MiniNDN lifecycle, and negative auth/replay/tamper/partial tests. |
| 9 | Promotion parity | Local/remote/staged SHA-256 equal; Tiger only verifies, stages once, and executes. |

Do not replace an earlier failed gate with a later smoke result. In particular,
`import ndnsf`, `READY`, visible CUDA, or a single successful Waf target does
not qualify the SIF. A physically usable SIF is usable only for the exact
source/lock/native-hash identity in its record.

## Operator sequence that prevents the recent failures

Use this order, without substituting a later expensive step for an earlier
failure:

```text
source/lock/definition seal
  -> disk and Apptainer-version gate
  -> complete Waf target census
  -> local complete SIF build
  -> in-SIF native/Python/library closure
  -> local CPU/ MiniNDN lifecycle and negative gates
  -> one hash-bound promotion
  -> Tiger hash verification and execution only
```

Before every Provider launch, enter the bundle (`cd "$BUNDLE"`), verify all
relative artifacts in the SIF, give each process an isolated HOME/PIB and
identity, and enforce Controller → bootstrap Provider → User ordering. Start
the Provider as `exec env ... di-native-provider` inside the background
subshell, then reap the tracked PID and verify that no job-local Provider
remains. A READY marker, an import, a single C++ target, or a visible GPU is not a service PASS;
the request must reach Request → ACK_CLOSED → Selection → Response with child
exit codes and evidence.

Classify these recurring symptoms before changing code or starting a remote
job: host `Python.h`/`cpXY` use is `WRONG_BUILD_BOUNDARY`; a sibling Waf link
failure means the target's own `use=` closure is incomplete; an old base
extension means stale-base replacement failed; missing relative artifacts mean
the Provider cwd is wrong; a stalled large fetch means the Face event loop is
not alive; Docker/OCI materialization means the wrong Spec170 route was chosen;
host Linuxbrew/`ld` contamination is `HOST_TOOLCHAIN_CONTAMINATION`; and
host/compute Apptainer or runtime-library drift invalidates the candidate.
Each repair creates a new candidate identity and preserves the failed
candidate's record.

## No-repeat operator record

For every update, retain one short record containing: candidate ID; source,
lock, definition, bundle, and workload digests; free-space and disposable-path
census; local/target-node Apptainer versions; build entry point; target-closure
census; SIF size/hash; `containerNativeBuild` and `hostBinaryInputs`; Python
`SOABI`/`EXT_SUFFIX`; Provider/framework/extension hashes; library-lock result;
exact local test commands/results; staged SIF hash; and the Tiger action
`verify-hash-and-execute-only`. Record the first failure and its class before
repairing it. The following classes are hard stops:

| Failure seen again | Classification and prevention |
|---|---|
| `Python.h`, `cpXY`, host venv, or host `.so` enters the image | `WRONG_BUILD_BOUNDARY`; build container-bound code inside the sealed SIF ABI. |
| One ONNX/Provider target links while a sibling misses NDN-CXX/NDN-SVS/ONNX Runtime | Incomplete target census; audit every target's own `use=` list before building. |
| Import passes but an older `_ndnsf.so` is selected | Stale-base contamination; remove old application binaries and require one final census/hash match. |
| Relative model/artifact is missing | Check both generated-bundle materialization and Provider cwd; require the file and matching `.sha256` sidecar, then enter `cd "$BUNDLE"` and recheck in-container. |
| Request passes but Provider processes remain | The workload tracked a shell instead of the Provider; add `exec env`, rerun, and require a zero post-run job-local process census. |
| READY but no Request/ACK/Selection/Response | Readiness is not service evidence; enforce isolated HOME/PIB and the explicit lifecycle barrier. |
| Linuxbrew/host `ld` resolves unrelated GTK/UAV/system libraries | `HOST_TOOLCHAIN_CONTAMINATION`; rebuild the complete target set in the sealed builder. |
| Duplicate SIF/model copies consume disk | Stop; keep one active SIF and content-addressed external model, then clean only enumerated paths. |

For DATA_V1 cancellation fault injection, keep the normal no-progress bound at
2000 ms. A deliberately injected stage delay must use an explicit
`--data-v1-no-progress-ms` value and record it in the run summary. The
successful delayed-stage MiniNDN evidence is
`real-minindn-cancellation-filterfix-20260819.md` in the repository evidence
directory; it is protocol evidence, not SIF promotion evidence.

This record is the operator memory aid; the active Spec170 ledger and the exact
candidate build record remain the acceptance authority.
