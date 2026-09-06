# Immutable Candidate Contract v1

## Candidate planes

One candidate binds exact digests for:

1. accepted Spec180 design, the Spec175 baseline handoff identity, and the
   exact Spec180 source delta from that baseline;
2. tracked source plus an explicit record of allowed dirty input bytes;
3. ndn-cxx, NDN-SVS, Boost, NAC-ABE, ONNX, ONNX Runtime, Python SOABI, compiler,
   CUDA runtime, and Apptainer build dependencies;
4. native libraries/binaries and Python extension;
5. SIF definition, build scripts, embedded runtime, and final SIF bytes;
6. focused tests, complete suites, MiniNDN harness, and exact-SIF replay driver;
7. Spec180 profile, renderer, submit wrapper, Slurm jobs, fixed environment,
   working directory, mounts, identities, routes, readiness, timeout, child
   supervision, and cleanup;
8. canonical YOLO/Qwen artifacts and their preprocessing/tokenizer/oracles;
9. registered invocation-input references, security domains, authorization and
   protection epochs, and plaintext-redaction rules;
10. registered local/Tiger workloads and validation/evidence schemas.

The candidate record must identify untracked or modified source bytes instead of
silently treating `HEAD` as the runtime source.

## Closure stages

### C0 — input closure before expensive work

The repository-owned closure command validates every source and configuration
plane, the Spec175 baseline handoff PASS, the recorded Spec180 source delta,
Spec180 audit PASS, full current-candidate local PASS, artifact
preflight, profile rendering, and expected output paths. It records the intended
SIF build input identity but does not upload, mutate remote storage, stage,
submit, or run a campaign.

Mutation tests must alter every bound plane one at a time and prove the closure
command exits nonzero before any external-side-effect adapter is called.

### C1 — local SIF seal

The host starts the Apptainer build, but every native/Python runtime artifact is
built inside the candidate SIF or an ABI-identical sealed builder. Never copy a
host-built `_ndnsf.so`, host virtual environment, host `site-packages`, or host
runtime library into the candidate. From inside the final image, record and
verify:

- Python executable and version, `SOABI`, `EXT_SUFFIX`, Python include root,
  compiler, glibc, CUDA, and ONNX Runtime identities;
- the imported `ndnsf._ndnsf` resolved path and SHA-256;
- every embedded NDNSF/NDN native binary and library SHA-256;
- RPATH and complete `ldd` resolution with no build-tree, host-home, missing,
  or unintended Linuxbrew dependency;
- CUDA and CPU execution-provider inventory, forbidden-package scan, and the
  real Spec180 runner `--help`/configuration probe.

Add the exact SIF SHA-256 and exact-SIF YOLO/Qwen replay results only after all
checks pass. A failure does not authorize remote upload.

### C2 — pre-dispatch closure

Render each fixed profile and verify it matches C1 plus the external model and
workload/input/security identities. Confirm the Tiger Apptainer path/version,
remote storage target, and resource availability read-only. Execute the same
inside-SIF import/runtime/configuration probes against the staged hash before
submission. Reject ambient/unregistered environment variables. Only C2 may
invoke upload/staging/submission.

### C3 — terminal closure

Accept only fresh results rooted under the registered run ID. Validate all
required protocol and result oracles, device/runtime evidence, every child exit,
and bounded cleanup. No marker or Response alone is sufficient.

## Single-subject and recovery rules

- Only one active candidate exists at a time.
- YOLO-F and QWEN-F are two gates of the same candidate, not two candidates.
- Evidence from Spec175 or another Spec180 candidate is never substituted.
- A Slurm/host failure before workload entry permits one byte-identical
  resubmission and preserves the first incident.
- Any changed source, dependency, artifact, workload, profile, wrapper, SIF, or
  oracle produces a new candidate and applies the invalidation matrix in
  `plan.md`.
- A workload-entry functional failure closes that gate as failed. Further
  redesign returns to the owning local task; it is not diagnosed by unbounded
  Tiger variants.
