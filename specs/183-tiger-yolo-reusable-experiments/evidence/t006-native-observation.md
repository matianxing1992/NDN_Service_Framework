# Native execution observation component

## Latest: independent container CUDA probe (2026-09-07)

`runtime/yolo_gpu_probe.py` calls the CUDA 12 runtime through ctypes, requires
exactly one visible device and resolves runtime ordinal0 via PCI to the CUDA
driver UUID. It rejects all-zero identities and query errors. There is no
model import, caller-provided UUID, nvidia-smi indexing, or fallback to CPU.
Its bounded JSON output is matched to a fresh launcher nonce and the exact
configured CUDA_VISIBLE_DEVICES. This is independent of Provider reporting,
not independent proof that Slurm allocated that physical GPU.

`NodeRuntime.probe_gpu_device` uses the same container_command and finite
process ownership as real role launches. It borrows an idle model-role HOME,
passes --nv and the node GPU selector, and records the finite child in cleanup.
Only exit0 plus exactly one bound result creates exclusive gpu-probe.json.
The record contains rank, borrowed role, nonce, observed UUID/selector, relative
log path/hash and CUDA_VISIBILITY_COMPONENT_ONLY. It contains no credentials.
The runtime module is in the required frozen harness inventory.

`run_normal_node` invokes this before network and Provider startup for both GPU
cases, under the existing remaining startup budget and peer-failure checks.
CPU cases skip it. Failure reaches the existing notification/cleanup path;
there is no retry or alternative GPU selection. The probe is not a public
submission bypass: external candidate gates and actual allocation verification
are still required before entering this internal node runner.

Tests use explicit CUDA ABI doubles (including a runtime0 to driver7 mapping),
actual CLI argument rejection and real finite subprocess ownership with a fake
Apptainer boundary. They do NOT execute CUDA or ORT. Model/graph verification,
Slurm job/step/hostname binding, immutable probe-receipt transfer/consumption
and live/retained allocation joins remain pending. T006/T007 are not complete.

Prerequisite bug found: finite nfdc/network probes borrow a Provider HOME but
do not exec the native Provider, so they have no Provider PID witness. Both
node receipt writer and reader now require that witness only for persistent
Provider launches (invocation=None), and reject unexpected witnesses on finite
commands. The first finite-management receipt regression failed with
NODE_RECEIPT_LAUNCH_NONCE before this fix; persistent-role validation is intact.

Expanded focused regression: 760 passed in 47.25s, with the same six Python
selectors plus TigerCluster/tests. JUnit:
Experiments/TigerCluster/results/t006-gpu-probe-r1/junit.xml.
No native build, SIF construction, transfer, or Slurm job was started.

## Latest: retained collector device enforcement (2026-09-07)

The retained request/dependency/role chain now calls validate_device_binding;
it no longer stops at native status and ORT profile agreement. GPU node entries
must carry external gpuBinding {uuid, visible}, dispatched to the model roles
on that rank only. Merge remains CPU even on a GPU node. Local CPU rejects a
node GPU binding; all CPU/Merge records must have empty GPU identity/visibility.

First red: test_retained_execution_uses_receipt_pid_and_scoped_profile with
cpu-gpu-exposure failed DID NOT RAISE, proving the real receipt/native/profile
reader accepted a contradictory CPU/CVD record. Fixed by invoking the device
validator in that consumer. Additional real-reader fixtures compare CUDA
UUID, selector and ordinal to external expectations, including missing and
mismatched bindings. They use synthetic GPU records, not GPU hardware.

Expanded focused regression: 738 passed in 47.90s. JUnit:
Experiments/TigerCluster/results/t006-device-collection-r1/junit.xml.
Selectors: TigerCluster/tests plus the six existing Python tests for V3 backend
selection, public recipients, YOLO numerical contract/reanalysis, candidate
identity and execution-plan identity. No native build or model execution.

Independent allocation receipt generation, its trusted transfer, the live
Worker collector's device join, certified graph coverage and final operator
remain incomplete. gpuBinding is an internal collector argument, NOT an
operator-supplied proof of allocation; no public command may manufacture it
from the same Provider log being verified. No Slurm/SIF/native qualification
was run for this change. T006/T007 remain open.

Allocation implementation constraint: Slurm's documented cgroup remapping can
make a task's CUDA_VISIBLE_DEVICES=0 refer to a device whose Prolog selector
was 1. NVML index and Linux device minor also need not coincide. Do not resolve
the task selector by indexing host nvidia-smi output. Measure the CUDA device
UUID in the actual allocated task context before launching Providers, and
bind it to the independently known job/step/node and container launch.
Reference: https://slurm.schedmd.com/gres.html#GPU_Management (GPU Management).

Workflow checks: Context Mode project/active health passed; CodeGraph index
current; Spec Kit pointer/prerequisites valid. GSD health has no errors but
warns that .planning/spec183-handoff.md is noncanonical (W019); the Spec183
tasks/evidence remain authoritative, not the older global GSD phase state.

Source audit: `ExecutionEvidence.cpp::executionEvidenceToJson` writes through
Boost PropertyTree `write_json`; scalar booleans/uint64 values are strings and
empty array trees serialize as `""`. `DI_NativeProviderExecutable.cpp` prints
that serializer directly after `NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED `.
The old Spec180 collector expects typed booleans and numbers and hardcodes
`/example/provider/<role>`. It cannot be reused unchanged for Spec183.

`runtime/yolo_result.py::decode_native_observation` now handles canonical
PropertyTree values only at the known scalar fields. Typed booleans/integers
remain supported, but bool-as-integer, whitespace, leading-zero, signed,
floating and overflowing uint64 strings are rejected. Parsing is bounded and
rejects duplicate keys, nonfinite constants and invalid array/assignment types.
No arbitrary string truthiness or recursive coercion is used.

`validate_native_observation` binds the decoded record to externally supplied
Provider identity, role, PID, request, attempt, plan and expected runner. It
requires completed load/warmup/execution, no exact forward cache hit or CPU
fallback, and model node assignments matching the intended CPU/CUDA backend.
Native Merge must have no GPU or node assignment. Status is explicitly
`NATIVE_OBSERVATION_COMPONENT_ONLY`.

29 focused tests cover canonical string/typed values, empty arrays, malformed
JSON/scalars, namespace independence and wrong/stale/failed execution bindings.
Fixtures match the source serializer shape; this is NOT an executed C++
serialization test or real Provider inference evidence.

Remaining: bounded log selection of exactly one observation per role/request;
read and independently validate ORT profile contents and model-node coverage;
bind physical GPU to allocation/rank/host; prove dependency edges and cleanup;
combine lifecycle, numerical and native components under frozen candidate
inputs. CPU and Merge acceptance need dedicated positive regressions as well.
T006/T007 and real local/SIF/Tiger gates remain open.

## ORT/profile and log follow-up

`read_native_observation` now selects exactly one configured role/request from
the launcher's bounded non-symlink log, rejects malformed observation JSON or
duplicates, validates its binding and retains a log hash. Missing or duplicate
observations never become PASS.

`validate_ort_profile` independently parses bounded non-symlink profile bytes,
extracts model Node events using the production applyOnnxRuntimeProviderProfile
rules (ignore non-node events and provider-free fence events; reject kernels
without provider identity), checks exact backend and ordered assignments
against the normalized native record, and returns a profile hash. Current
request/attempt must match profile lineage; status is ORT_PROFILE_COMPONENT_ONLY.
File reads use the existing 4 MiB harness reader limit, and JSON event counts
are additionally bounded. Caller must map the native container path into that
Provider's owned output, not accept arbitrary host paths from logs.

Added positive CPU/Merge record tests, CPU profile/fence test, GPU profile
matching, stale-request/attempt/missing/extra/misnamed-node/fallback/symlink
negative tests and log cardinality tests. Combined profile/native subset:
46 passed. All profiles are synthetic test data, not real ORT execution proof.

Source caution: OnnxRuntimeModelRunner captures profiling once per session
(`profilingCaptured`), and binds profileRequestId to that run. Do not assert
that every subsequent request has fresh profiling. The actual session reuse
path must be checked before warm qualification; until then the collector
rejects stale profile lineage rather than claiming current execution from it.
Full model-node coverage against the certified assembled graph, physical GPU
allocation, dependency transfers, complete cleanup and orchestration remain.

ORT/log follow-up expanded regression: 536 passed in 42.22s with the same six
selectors; JUnit `Experiments/TigerCluster/results/t006-ort-profile-r1/junit.xml`.

Initial expanded regression: 517 passed/1 failed (missing-peer test received
Python 3.8 asyncio.TimeoutError rather than built-in TimeoutError). Normalized
that API boundary and added deterministic outer-timeout injection coverage.
Focused network+observation rerun: 43 passed. No timeout budget was relaxed.

Final expanded regression: 519 passed in 39.09s, JUnit
`Experiments/TigerCluster/results/t006-native-observation-r2/junit.xml`.
Selectors: TigerCluster/tests plus test_spec183_v3_backend_selection,
test_spec183_public_recipients, test_spec180_yolo_numerical,
test_spec183_numerical_reanalysis and test_spec183_candidate_identity under
tests/python. No native/SIF/Tiger execution was performed.
# Request profile lifetime audit (2026-09-07)

The ONNX adapter captures profiling once per runner session, not once per
arbitrary subsequent Run on a reused session (`profilingCaptured`). Do not
attribute an old profile to a later request. In the configured V3
post-Selection path, NativeProviderHandler.cpp around 2195 calls
runnerPreparationFactory and runnerFactory->create for each invocation.
RegistryNativeModelRunnerFactory::create dispatches the creator; the ONNX
creator constructs a new OnnxRuntimeModelRunner. NativeRunnerPreparation.cpp
sets a unique PID/sequence profile prefix and profileAfterRequest=true.
Thus this source path supports strict request/attempt profile matching without
weakening the collector. Native execution must still confirm this wiring.

The schedule's initial warmup request must not be described as proof that later
requests reuse a loaded ORT session: process/artifact caches and runner/session
reuse are different. If a future factory starts caching runners, qualification
must address per-request execution evidence explicitly rather than passing the
first request's profile off as current evidence.
# PID namespace finding and witness (2026-09-07)

Follow-up wiring supersedes the pending implementation note below. Each real
Provider Worker launch now creates a fresh nonce and uses the frozen witness
module inside the container before execing di-native-provider. Node receipt
v3 stores launchNonce along with the host PID. Both receipt production and
offline reading require exactly one marker with matching nonce/role and a
positive integer namespace PID. Live/retained native collectors pass that
nonce and validate native processId against the witness PID, not the host PID;
hostProcessId is separately retained for ownership/cleanup provenance.

The new real unshare test executes the witness and an application that emits
native-format evidence with its own os.getpid(): host-PID-only validation
fails, nonce-bound validation passes and retains the distinct host PID. The
record is still synthetic compute evidence, but PID isolation/exec and the
reader are real. Wrong/missing/duplicate witness, wrong nonce/role and boolean
PID tests reject. Fake-Apptainer launcher tests now emulate /bundle Python
module lookup; they remain explicitly fake and do not replace exact SIF.
73 affected tests passed with no skips. Exact-SIF native acceptance remains
mandatory before promotion; no isolation setting was removed.

Expanded focused regression: 693 passed in 54.62s, no skips; JUnit:
`Experiments/TigerCluster/results/t006-pid-binding-r1/junit.xml`.

Installed Apptainer exec --help states --containall isolates PID, IPC and
environment. Current container_command uses this option; ExecutionEvidence
records getpid(). Therefore comparing it directly with the host Popen PID is
incorrect, even if the non-container fixture passes. This is a T007 blocker
until the actual launch/receipt/collector path is corrected.

runtime/yolo_launch_witness.py emits TIGER_PROVIDER_PROCESS_STARTED with a
64-hex host-generated nonce, role and namespace PID, then os.execvp replaces
the process without changing that PID. It is included in the required harness
inventory. Six actual-process tests passed, including a real user/PID
namespace test showing host PID differs while witness PID equals application
PID; no skip on this host. This is not exact-SIF qualification and not
authentication against a malicious container. The host-owned output FD,
fresh nonce, receipt and native evidence must be joined by the collectors.

Pending next: generate/retain nonce in Worker, launch through the witness,
persist it in node receipts, require exactly one matching witness and use
its namespace PID for native identity while retaining host PID for cleanup.
Do not accept native logs by copying their reported processId into expected
values. Existing PID-equality collectors are not yet repaired.

Expanded focused regression: 686 passed in 46.52s, including the real PID
namespace test with no skips. JUnit:
`Experiments/TigerCluster/results/t006-pid-namespace-r1/junit.xml`.
# Device allocation binding component (2026-09-07)

validate_device_binding requires expected UUID and visibility selector from
an independently verified allocated-node preflight and actual launch. Native
CUDA evidence must match both UUID fields, exact CUDA_VISIBLE_DEVICES, device
kind cuda/ordinal 0 (single exposed device), and gpuIdentitySource equal to
cuda-runtime-pci+driver-uuid. The source implementation queries the CUDA runtime
PCI identity then the driver UUID (CudaDeviceIdentity.hpp) and stores these
fields in OnnxRuntimeModelRunner; environment text alone is not that evidence.
Uppercase UUID selectors allowed by container_command remain supported, while
native UUID is compared to its canonical lowercase-hex representation.

CPU and native Merge reject nonempty GPU UUIDs, CUDA visibility or GPU source;
no GPU expectation may be supplied for these roles. Thirty-six tests use
source-shaped device records and reject wrong UUID/list/ordinal/selector/source,
missing device information, ambiguous expected selectors and CPU exposure.
They do not probe a GPU or qualify a Slurm allocation. The function returns a
component-only status; trusted allocation receipt production, role-wise wiring,
physical host agreement and final operator integration remain pending.

Expanded focused regression: 729 passed in 55.70s; JUnit
`Experiments/TigerCluster/results/t006-device-binding-r1/junit.xml`.

# Model identity follow-up (2026-09-07)

The retained public assignment envelope is now `yolo-public-assignments-v2`.
Each role records `modelManifestDigest` and `artifactDigest` from the typed V3
assembly. The User writer rejects incomplete identity; the offline reader
requires the v2 envelope, canonical digests, and exact role ownership. The
expanded focused suite recorded 824 passing tests in
`results/t006-model-binding-r1/junit.xml`; the subsequent 840-test suite also
passed in 47.69 seconds (`results/t006-graph-coverage-r1/junit.xml`). This
establishes a fail-closed
identity boundary only. It does not authenticate the model package by itself,
and it does not prove every certified graph node ran: ORT optimization can fuse
nodes, so coverage must use a separately certified optimized-node mapping.

The collector now has a fail-closed optional join for
`tiger-yolo-certified-graph-v1`: the graph document is supplied externally and
binds each role's model/artifact digests, backend, and optimized node names.
Provider logs cannot create or extend this expected set. The final operator must
require this join (rather than leaving it optional) before T006 is complete.

The component-only path now owns the complete normal request loop through
`collect_normal_verdict`, then applies the fail-closed
`finalize_normal_verdict` boundary. It accepts only the registered request
schedule after lifecycle, numerical, execution, four-role certified-graph, and
case-specific device checks all pass, and emits
`tiger-yolo-final-verdict-v1`. The operator CLI is not yet wired to real
retained paths; this remains a component regression guard, not native or Tiger
evidence.
