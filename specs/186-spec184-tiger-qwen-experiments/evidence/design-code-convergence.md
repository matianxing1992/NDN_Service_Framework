# Spec186 design-to-code convergence audit — checkpoint 1

**Date:** 2026-09-12
**Candidate scope:** profile and lifecycle implementation on `SPEC184Experiments`; source seal `575b43cc93bbed29932303caf3d09974f1585af7`.
**Verdict:** `PASS (implementation convergence); BLOCK (runtime qualification)`

## Verified wiring

| Contract | Code/evidence | Result |
| --- | --- | --- |
| Profile schema and case registry | `Experiments/TigerCluster/runtime/spec186_candidate.py::load_profile`; four declared profiles | focused mutation suite passes; unknown fields and duplicate JSON fail closed |
| Candidate tuple and invalidation | `build_candidate_manifest`, `earliest_restart_gate` | deterministic digest and external-model restart test pass |
| Pre-dispatch boundary | `pre_dispatch` | no SSH/rsync/staging/sbatch calls; receipt reports zero counters |
| Effective argv/env/binds | `jobs/spec184/submit.py::render_effective` | case, candidate digest, NFD transport, role map and read-only binds are explicit |
| Scheduler ownership | `jobs/spec184/submit.py::submit` | scheduler call is after pre-dispatch only; invalid candidate test observes zero calls |
| Process ownership/cleanup | `local_run`, `jobs/spec184/run.sbatch` | one process group, finite request/cleanup budgets, reaping receipt; focused terminal tests pass |
| Native business ownership | existing `NDNSF_DI_YoloAckDriven_Minindn.py` and `NDNSF_DI_Qwen06B_Native_Minindn.py` are referenced as harness launchers | Python adapter passes through to native entrypoints; no model logic added |

## Controlling gaps

1. The current host's built `di-native-provider` exits with an unresolved
   `DeploymentControlMessage` vtable, and importing `_ndnsf.so` selects an
   incompatible `/usr/local/lib/libndn-service-framework.so` with unresolved
   `ndnsd::discovery` symbols. `readelf -d`/`ldd -r` reproduce this loader
   boundary. T006 must rebuild in the locked dependency environment and seal a
   matching base SIF/app bundle before this audit can pass.
2. No Qwen3-0.6B weights/tokenizer are present. The only discovered GGUF is a
   Qwen2.5 0.5B file and is intentionally rejected as a substitute. T008 is
   `WAITING_EXTERNAL_INPUT` until a compatible model/backend tuple arrives.
3. This local host has no `sbatch`, `nvidia-smi` or installed `minindn` command.
   Tiger single/two-node execution and local MiniNDN execution therefore have
   no runtime evidence yet. T007 and T009–T012 remain open.
4. The Tiger `run.sbatch` payload now executes the explicit rendered argv, but
   a real Tiger staging receipt must prove that the launcher, app bundle,
   base SIF and model paths resolve inside the declared run root. A scheduler
   payload echo is not a qualification result.

## Closing action

Run the T006 clean build/ABI closure with packaged dependency paths first,
materialize the matching base+application layers, then rerun this audit. A
`PASS` requires the native import and `--help` checks to succeed and the
effective launcher to resolve in the same candidate tuple; until then no
MiniNDN or Tiger status may be promoted.

## Checkpoint 2 after profile expansion

The case registry now contains all eight contract cases, including explicit
Qwen stage order/dependency metadata and signed GPU-capacity placeholders.
`test_spec186_candidate.py` passes 11 focused tests and the effective launcher
maps YOLO to `--case Y-A/Y-N` and Qwen to the native stage-manifest entrypoint.
The verdict remains `BLOCK`: these structural changes do not replace the
missing ONNX toolchain, unresolved native ABI, Qwen3 artifact, local MiniNDN
inputs or Tiger scheduler/GPU evidence listed above.

## Checkpoint 3 after dependency probes

The Python ONNX 1.17.0 package was sufficient for a bounded temporary
full-protobuf archive probe, but the result is outside the repository and is
not source-sealed. Waf reconfiguration with that probe and the matching
NAC-ABE prefix passed dependency discovery before stopping at the absent pinned
Rust tokenizer toolchain and offline cargo home. Existing native artifacts
still fail the import/`--help` closure, so the verdict remains `BLOCK` and no
runtime or Tiger result is promoted.

## Checkpoint 4 after all-profile pre-dispatch

Offline `prepare/check` produced a deterministic candidate manifest for each of
the eight cases; the receipts are in
`evidence/pre-dispatch-boundary-20260912.md`. Every invalid or externally
incomplete tuple was rejected with zero SSH, rsync, staging or scheduler side
effects. The first local runtime boundaries remain the missing base SIF/native
loader closure for YOLO and the missing Qwen3 model/stage/tokenizer tuple for
Qwen. This strengthens the dispatch gate but leaves the audit verdict `BLOCK`.

## Checkpoint 5 after native rebuild and checker repair

The exact NDN-SVS source/build pair, official NAC-ABE prefix, temporary ONNX
protobuf prefix and pinned Rust 1.90 bridge cache now produce the framework/DI
shared libraries, ONNX assembly worker and a clang-built native provider. The
Python extension was rebuilt against the same library directory; canonical
`import ndnsf._ndnsf` passes in a clean process and its static `ldd -r`
command returns zero. The pre-dispatch checker now performs that canonical
import in a subprocess; its previous arbitrary module alias caused a false
pybind initializer failure.

The verdict remains `BLOCK`. The declared Spec186 base SIF is not present on
this host, and the baseline provider parser returns status 2 for `--help`
after printing usage. The contract requires a zero-status help probe, so the
source or entrypoint must receive an explicit sealed repair before T006.b can
pass. Qwen3-0.6B assets, MiniNDN runtime inputs and Tiger Slurm/GPU evidence
remain external prerequisites.

## Checkpoint 6 after provider help repair

The provider parser now accepts `--help`/`-h`, exits before requiring a plan or
manifest, prints usage and returns zero. The changed translation unit compiles
with clang 10 and the current source headers. A retained `build/` object-tree
relink was deliberately rejected because it mixes older Core/DI and
NDN-SVS/NAC-ABE ABIs; it does not provide a candidate or runtime help receipt.
The convergence verdict remains `BLOCK` until the locked dependency tuple is
rebuilt and the new provider passes the full import, entrypoint, RPATH and
`ldd -r` closure together with an exact source-sealed base SIF/application
composition. The Qwen3, MiniNDN and Tiger execution prerequisites are
unchanged.

## Checkpoint 7 — 2026-09-13 native closure and layered app receipt

The source repair and all native consumers were rebuilt in a fresh
`build-spec186-r4` tree. The provider `--help`, requester `--help`, authority
`--help`, canonical `_ndnsf` import, official Spec180 identity verifier,
`readelf -d`, RUNPATH and native `ldd -r` checks now pass together. The
application layer has a content-addressed local bundle with digest
`badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d`.

This closes the implementation convergence items that were controlled by the
provider entrypoint and stale native objects. The qualification verdict stays
`BLOCK`: the bundle is not paired with an exact Spec186 source-sealed base SIF,
the local host has no MiniNDN runtime, and Tiger still needs staged NFD
identities/routes and external normal/negative receipts. Qwen3-0.6B remains an
external input. No component receipt is promoted to a protocol or GPU PASS.

## Checkpoint 8 — 2026-09-13 collector identity repair

The eight Spec186 profiles were still carrying the pre-directory-digest
collector hash `a7cf3577…`, which made every pre-dispatch attempt fail before
asset checks even though the runtime implementation had already changed. All
profile `runtime.harness.collector` and `evidence.collector` entries now bind
the current `spec186_candidate.py` digest
`b9bb5f886fc2ee39ab50f7d85814974c595d866ef1962164968165dad9697291`.
JSON/profile validation and the full TigerCluster suite pass (`74 passed`).

This repairs candidate identity convergence only. The profiles still fail
closed on their intentionally absent exact base SIF, app paths and external
model inputs; the runtime qualification verdict remains `BLOCK`.

## Checkpoint 9 — 2026-09-13 source and app path binding

The profiles now bind the provider-repair source commit and source seal used by
the fresh native build, plus the immutable nine-file application bundle. Local
MiniNDN profiles use the host bundle directory; Tiger profiles use its verified
project-storage staging path. Their entrypoints resolve to
`bin/di-native-provider`, and all eight candidate manifests were regenerated.
The new pre-dispatch receipts show the collector mismatch is gone and retain
zero remote side effects. Runtime qualification remains `BLOCK` until the
matching source-sealed base SIF is built and visible in the selected execution
environment.

## Checkpoint 10 — 2026-09-13 Apptainer runtime pin and r5 digest repair

All eight profiles now bind `runtime.apptainer.version=1.5.3` and an explicit
executable: `/usr/local/bin/apptainer` for local MiniNDN/SIF work and
`/usr/bin/apptainer` for Tiger compute jobs. The launcher uses that declared
path rather than PATH lookup, local pre-dispatch probes `--version`, and the
compute preflight rejects versions outside the 1.5.3 package line. The login
node's 1.3.4 package remains metadata-only. The profiles also now match the
actual r5 tree digest `687610de…07129` and the current collector digest.

The focused profile tests pass (`14 passed`) and the full TigerCluster suite
passes (`76 passed`). This closes the runtime-version identity gap but does not
close the absent source-sealed base SIF, repository route, model, MiniNDN or
Tiger campaign gates.

## Checkpoint 11 — 2026-09-13 streamed collaboration runtime repair

The first real four-Provider MiniNDN run reached ACK/Selection but failed
because the request-scoped Provider path required an event-key grant for every
collaboration role. The sealed V3 plan marks only Stage/3 as the terminal
response owner. The repair splits streamed multi-Provider Selection into
Provider-specific messages, initializes a publisher only for a present grant,
and allows an absent grant only for a registered collaboration assignment.
The rebuilt runtime and TigerCluster suite pass; r53 then records non-terminal
dependency execution, terminal grant acceptance, two token events and clean
MiniNDN teardown.

This closes the controlling implementation gap exposed by r48/r49/r51, but is
only a local framework regression receipt. The implementation verdict remains
`PASS`; exact source-sealed SIF, YOLO Y-A/Y-B/Y-N, Qwen3 and Tiger execution
remain separate blocked or waiting gates.

## Checkpoint 12 — 2026-09-13 current-source r6 identity refresh

The collaboration repair was rebuilt from source commit
`6d143d3f0f7a7c627af2c1ef6810d79c0738b52d` with source seal
`sha256:f4676a0f937c903caebc8374893171890d0d6d0639be42a3ced1b101d7512a00`.
The nine-file read-only r6 application bundle has tree digest
`04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73` locally
and on Tiger project storage. All active profiles now bind that r6 digest and
Apptainer 1.5.3; no local 1.3.4 executable remains, while the login node is
outside the SIF execution path. Native identity and focused regressions remain
green. The exact source-sealed 1.5.3 base SIF and composition receipt are still
the controlling T006.c gate.
