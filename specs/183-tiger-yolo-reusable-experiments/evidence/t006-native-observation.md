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
