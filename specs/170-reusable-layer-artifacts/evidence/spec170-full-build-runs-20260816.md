# Spec170 full sealed-build attempts (historical, 2026-08-16)

> **Historical ledger only.** These are prior Tiger-side build attempts. They
> are retained for failure provenance and are not the current release route.
> New candidates must be built and validated locally, then promoted as one
> hash-verified SIF; see `local-sif-build-route-20260817.md`.

This ledger separates source/build identities and preserves the first failure
for each bounded attempt. A SIF is accepted only when the exact source seal,
OCI digest, SIF hash, static probe, and release manifest all agree.

## r1--r3 negative evidence

| Job | Source identity | Result | Failure point |
|---|---|---|---|
| 195823 | `2852375df234c58a0624b51d1c6b85b2f3f13c4b` | FAIL | static probe attempted to create `/home/tma1/.ndn` on the read-only SIF root |
| 195965 | `593b04af95253777ab76acbdb877e763e4f4b422` | FAIL | `APPTAINERENV_HOME` overrode the probe HOME; `/home/tma1/.ndn` remained read-only |
| 196129 | `33d66b74b3dd0ba6ebc82b139fddde9a7637497d` | FAIL | `--cleanenv`/unset site variables still did not prevent the same read-only HOME resolution |

The r3 build did produce a SIF before the probe failed. Its manifest and
failure logs remain on Tiger; it was not promoted as a release.

## r4: complete SIF, failed acceptance probe

Job `196296` used source commit `f55da0e78d22dded9dfd0ae5b61320e455abfde1`
and source-seal digest
`sha256:8b7616b0142effb8f9857aef5b7ab1d6840d698ec17d8465d1ead5dcf57fa232`.
The complete OCI and SIF materialization succeeded, but the static probe
failed with:

```text
WARNING: Overriding HOME environment variable with APPTAINERENV_HOME is not permitted
filesystem error: cannot create directories: Read-only file system [/home/tma1/.ndn]
```

The durable manifest classifies this as
`ROOTLESS_BUILD_SIF_EXEC_FAILED`, exit code 5, with `sifSha256` recorded as
`sha256:e295bdcd58106426eece2e96da56d181ed167d3a8550491753573b0a78cd5e18`.
No SIF was promoted. This is a probe-environment failure, not an OCI build
or disk-capacity failure.

## r5: corrected probe binding

Candidate commit `0142d4ee2310de056aa391d954f2c3db8fb62023` changes only the
probe wrapper: it creates a fresh job-local directory and binds it at the
exact `/home/$(id -un)` path while keeping `env -i`, `--cleanenv`, and
`--no-home`. The source archive and dependency checksums are:

```text
workspace.tar sha256:6ca707aebb06bae6e6e3861d061942d9d6a148c2eda9e944a26e86b092a5768f
source seal   sha256:0b2ea4a56713d4a2b4e36bdc47edba568cfcb7c4dab7e1db3b2bf198d5ac5462
rootless tool sha256:ff8788d4df124d5b09fcfe9fe1da43eaaa83df29ccc89c872602bcd0773ce1b5
preflight     sha256:256cd4bde6c0e15efce45ad65b5866b0d798d93c2cb264a8f413f7c90ed42e13
```

Tiger job `196525` produced the complete release
`spec170-runtime-0142d4-gpu-20260816-r5` and its static probe printed
`status=PASS`; the durable `SHA256SUMS` check also passed. The job nevertheless
returned exit code 6 because rootless Buildah left ownership-protected files
in its disposable scratch graphroot:

```text
reasonCode=ROOTLESS_BUILD_SCRATCH_CLEANUP_FAILED
status=FAIL
releasePath=/project/tma1/ndnsf-di/releases/spec170-runtime-0142d4-gpu-20260816-r5
runtime.sif sha256:490ff5fbf20ef3be56caf398478f457efc2a786fdeaf41e90d1ccbbc9addafb6
runtime.oci.tar sha256:bf2c73d5086bbec15d4a6c2e85e0e6d7bed136c2d75dfa42aa93cf378dd96422
```

This is classified as `artifact-ready / orchestration-failed`, not a clean
materialization PASS. The release manifest, SIF, OCI digest, and failure log
are retained. Independent exact-SIF Tiger job `196669` subsequently verified
the same SIF by path and SHA without rebuilding it and passed the static
runtime/Python/ORT/Torch checks. Gate-C may therefore use the exact SIF for a
bounded current-SIF network smoke, while the wrapper cleanup defect remains a
packaging follow-up.

## r8/r8b: staging-layout correction and artifact-ready build

Job `197147` is retained as a pre-dispatch negative: the source seal was valid,
but the final staging root omitted the dependency mirror consumed by
`rootless-build.sh`, producing `ROOTLESS_BUILD_DEPENDENCY_ARCHIVE_MISSING:NAC-ABE`.
The corrected staging root passed the local and remote sealed-source validator
with the same values before the retry was submitted.

Job `197148` then completed the full 333-target build, Python environment
contract, wheel build, OCI commit, SIF conversion, and static runtime probe for
source commit `07738d708d36596a14f1988db4395f1c2bf33fcd`. The promoted SIF is:

```text
release: spec170-runtime-07738d70-gpu-20260816-r8b
runtime.sif sha256:6ca4fe45fa0c6669ec72e9af8b671db385f352c1076614f394401d3d05cbdc31
size: 4,398,755,840 bytes
static probe: PASS
```

The Slurm job returned exit code 6 because rootless Buildah left ownership-
protected files in disposable `/tmp` scratch
(`ROOTLESS_BUILD_SCRATCH_CLEANUP_FAILED`). The SIF and `SHA256SUMS` verify
independently, so this is classified as `artifact-ready /
orchestration-failed`; it is not a rebuild or a runtime-probe failure. The
next gate is a bounded D0 CPU network request/ACK/Selection/Response run
against this exact SIF.

The first D0 submission (`197149`) is retained as a launcher-negative: the
host release path was passed as the workload argument, but the container
wrapper exposes that tree at `/release`. Apptainer consequently returned
`rc=127` for a missing `/project/.../spec170-d0-current-sif-workload.sh`.
This is a path-contract error, not an image or NDNSF execution failure. The
retry uses `/release/d0-current-v2/spec170-d0-current-sif-workload.sh` and a
new evidence directory.

The corrected container-path retry (`197150`) is retained as a second launcher
negative because the default bind includes the release-id directory. The
successful D0 submission is Job `197151`, using
`/release/spec170-runtime-07738d70-gpu-20260816-r8b/d0-current-v2/spec170-d0-current-sif-workload.sh`.
It completed with exit code 0 and reported:

```text
SPEC170_D0_CURRENT_NETWORK_PASS
request_ack_selection_response=PASS
user_rc=0 response_ok=1
roles: /Backbone, /Head/Shard/0, /Head/Shard/1, /Merge
requestCount=1 successCount=1 failureCount=0 elapsedMs=84.1666
runtime: ONNX Runtime 1.20.0, runner=onnxruntime-cpu, cpuFallbackUsed=false
```

Each of the four native Providers emitted one backend-ready, ACK-decision, and
final-response record. This closes the r8b D0 CPU network gate for the exact
SIF; GPU and multi-node gates remain pending.

Initial D1 network submission `197152` was cancelled after 7 minutes with no
workload evidence while the Apptainer/CUDA process was still loading. It is
not a protocol result. A separate exact-SIF `allocated-gpu` runtime preflight
must pass before resubmitting D1, isolating CUDA/ORT startup from the NDNSF
request path.

The exact-SIF runtime preflight (`197160`) was therefore submitted first with
a 10-minute limit and then cancelled at 4:53 after producing no probe JSON.
At cancellation, the batch step had only 3 seconds of CPU time but had read
about 984 MB from the SIF and reached 344 MB RSS. This is a bounded
Apptainer/SIF launch-path failure on `itiger07`, not a CUDA protocol or NDNSF
network result. D1 remains blocked until a node/path passes the same preflight.

After tool v3 passed the GPU runtime preflight (`197163`), D1 network Job
`197164` reached a real CUDA Provider and loaded all four roles with
`runnerKind=onnxruntime-cuda`, `cpuFallbackUsed=false`, but the workload's
default selection generated an invalid assignment role and returned
`Provider is not authorized for collaboration role /Inference/NativeTracer`.
This is a workload mapping defect, not a GPU or SIF failure. The retry uses an
explicit `/Backbone`, `/Head/Shard/0`, `/Head/Shard/1`, `/Merge` to single
Provider mapping and records a new job identity.

Jobs `197165` and `197166` are retained as wrapper negatives: the first
rejected a brittle source-text marker and the second generated an incomplete
shell command, so neither is protocol evidence. Job `197167` used the
corrected explicit role map and reached all four CUDA Providers with
`cpuFallbackUsed=false`, but the request still failed with
`Provider is not authorized for collaboration role /Inference/NativeTracer`.
This is not a GPU, SIF, or launch-path failure. A pre-dispatch comparison of
the two staged bundles showed why it should have been stopped earlier: the D0
and D1 `native-execution-plan.json` hashes matched, but
`service-manifest.json` differed (`10f10ee...` vs `8a2598f...`) and
`user_driver.py` differed (`cfb9284...` vs `207969a...`). The workload was
therefore stale relative to the candidate even though its plan looked current.
No further D1 retry is justified until a fresh workload is generated from the
current D0 bundle and its complete file-hash/role-map preflight passes.
The new checker reproduced this as `status=FAIL` before any allocation; its
retained JSON is `spec170-network-bundle-preflight-20260816.json`.

Job `197168` is retained as a scheduler-placement negative. Although the
bundle preflight passed, Slurm placed the job on `itiger07`, the node already
marked unqualified after the exact-SIF launch stall in Job `197160`. The job
was cancelled after 14 seconds before protocol evidence was produced. The D1
batch file now carries `#SBATCH --exclude=itiger07`; node qualification is
persisted independently of the SIF and workload hashes.

Job `197169` reached the healthy `itiger11` node and the CUDA-capable Provider,
but failed before READY because the newly generated D1 script did not change
into its bundle directory. The manifest uses relative artifact names, so the
Provider reported `Load model ... artifacts/qwen-native-tracer-backbone.onnx
failed. File doesn't exist`. This is a workload cwd/mount defect, not a
runtime or NDNSF protocol result. The script now launches the Provider inside
`cd "$BUNDLE"`, and the bundle checker rejects relative-artifact workloads
without that cwd contract.

Job `197170` used the corrected cwd and loaded all four artifacts, but it
revealed a configuration distinction that must not be hidden by the SIF
wrapper. The current D0 manifest has no `executionProvider` field, so the
native ONNX runner correctly defaults to `onnxruntime-cpu`; it is not a CUDA
workload merely because the SIF was launched with `--nv`. The Provider reached
READY and emitted a valid ACK, but the request still ended with
`Provider is not authorized for collaboration role /Inference/NativeTracer`.
This run is therefore a configuration/assignment negative, not evidence of a
broken SIF or CUDA image.

The run configuration is now simplified and frozen as follows: one immutable
SIF (`runtime.sif`, SHA-256
`6ca4fe45fa0c6669ec72e9af8b671db385f352c1076614f394401d3d05cbdc31`), one
source-aligned D1 bundle, and one direct workload (no wrapper-generated text
patch). D1's `service-manifest.json` is deterministically derived from the
current D0 manifest by adding `executionProvider=cuda`, `deviceId=0`, and
`allowCpuFallback=false` for the four NativeTracer artifacts. The plan,
Python driver, policy, trust schema, and artifacts remain byte-identical to
D0. The bundle checker permits only this intentional manifest difference and
requires all four role mappings plus the bundle working-directory contract.
The resulting remote preflight for
`network-bundle-d1-cuda-current` returned `status=PASS`; no new D1 job has yet
been submitted after this simplification.

Job `197171` was then submitted from that exact preflighted SIF/bundle/workload
triple. It reached `itiger11`, loaded all four artifacts, and emitted
`NDNSF_DI_EXECUTION_EVIDENCE` with `runnerKind=onnxruntime-cuda`,
`realCompute=true`, `device.kind=cuda`, and `cpuFallbackUsed=false`. The
Provider became READY and published a successful ACK. The request nevertheless
failed before execution with
`Provider is not authorized for collaboration role /Inference/NativeTracer`.
The user trace shows one Provider entry containing all four roles, so this is a
reproducible NDNSF multi-role assignment decoding defect: the selection payload
is an `OpaqueAssignmentSet`, but the Provider previously treated the container
as an unstructured payload and fell back to the service name as its role.

The local fix parses a multi-envelope `OpaqueAssignmentSet` as one aggregate
same-Provider execution context, merges the per-role scope-key references, and
derives every `roleProvider.<role>` field from the set. It preserves the
single-role path and does not add a wrapper or alter the SIF. Focused local
tests for opaque assignment sets and native assignment validation pass; a new
source seal and SIF are required before repeating D1. Until that rebuild, the
immutable `197171` SIF remains a valid CUDA/runtime negative but not a protocol
failure of the image.

The follow-up wire-trace diagnostic `197192` used the same immutable SIF and
bundle and retained the exact Selection path. The User selected one Provider
for four roles and attached four role envelopes; each Provider-specific
Selection carried a 6412-byte `OpaqueAssignmentSet`, and the encrypted payload
was 6813 bytes. The Provider received and decrypted that payload, then rejected
it only after the local projection path appended semicolon metadata to the
binary set. This rules out missing provider entries, NDN wire truncation, SIF,
CUDA, and artifact-path causes. The minimal fix now detects a multi-envelope
structured set and skips both legacy metadata concatenation branches while
retaining the old single-envelope/text behavior. The new local regression
passes together with all ten `GenericOpaqueSelection/*` cases. A new sealed
source/SIF identity is still required before D1 is rerun.
