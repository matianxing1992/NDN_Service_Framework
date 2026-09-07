# Native execution observation component

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
