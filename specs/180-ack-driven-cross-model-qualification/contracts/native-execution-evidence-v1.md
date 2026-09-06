# Native request execution observations

Owner: T007/T013. Implements the producer portion of FR-018/019, not terminal
qualification. Reuses `ndnsf-di-execution-evidence-v1` and its existing observer;
does not introduce a wire protocol, Provider process, model backend or planner.

## Observation owners

- The runner-spec evidence factory records actual `getpid()` and the process's
  `CUDA_VISIBLE_DEVICES` value. Visibility is configuration evidence, not proof
  of computation. Caller metadata and `NDNSF_DI_GPU_UUID` do not supply UUIDs.
- The CUDA adapter uses its selected runtime ordinal to query PCI bus identity,
  then resolves that address through the CUDA driver and queries the physical
  UUID. It records `gpuIdentitySource=cuda-runtime-pci+driver-uuid`. Missing
  libraries/symbols, failed queries or an all-zero UUID fail closed. CPU runners
  and native Merge do not invoke this query. No new toolkit link is required.
- V3 post-Selection ONNX assembly sets a profile prefix in its artifact cache,
  unique per PID and process-local sequence, and `profileAfterRequest=true`.
  Profiling includes constructor warmup and the first authenticated request;
  warmup alone does not finalize it. After the request's ORT run, record
  `profileRequestId` and `profileAttemptEpoch` with the profile assignments.
- Required CUDA disables ORT CPU EP fallback. CUDA evidence rejects non-CUDA
  node assignments and kernel events lacking a provider. Non-kernel fence
  events without a provider are not treated as computation evidence.
- After a runner returns (or a cache lookup succeeds), ProviderRoleWorker binds
  `requestId`, `attemptEpoch`, `processId`, visibility, `exactForwardCacheHit`
  and `executionCompleted` on the per-result snapshot, not the shared runner.
  Completed means fresh runner execution with nonempty request ID and nonzero
  attempt; cache hits and unbound legacy calls cannot claim it. This does not
  prove dependency publication, protocol success, numerical correctness or
  cleanup. `runnerKind`/`realCompute` remain independently mandatory.

## Compatibility and consumption

New fields are additive. Old v1 JSON remains readable with defaults: zero IDs,
empty strings and false flags. Serialization retains Boost property-tree's
existing string scalars; a consumer must parse booleans/integers strictly,
never use Python `bool("false")`. The older Python readiness dataclass discards
these extra observations; it must not be used as the qualification collector.

The executable emits `NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` followed by the
actual per-role JSON snapshot. Existing aggregate UPDATE/readiness records
remain, but multi-role aggregation clears request-completion/profile binding
rather than pretending several snapshots are one execution. The Spec180
collector must consume the original observed records and compare their role,
Provider, boot ID, PID, request, attempt, plan and artifact identities against
the authenticated lifecycle and observed child inventory.

For the three CUDA roles, require a complete, non-cache observation and a
profile request/attempt matching the same cold request; require nonempty
CUDA-only node assignments, no CPU fallback, the queried physical UUID and
expected visible-device mapping. CPU Merge requires its own native runner
observation without CUDA claims. Bind the actual profile file path/hash, not
only its summary. The profile is first-request evidence, not proof of every
later request; mismatched/reused profile identities must fail qualification.

The runtime record still needs the numerical/protocol/redaction/cleanup
components and immutable candidate binding. Startup, warmup or this marker
alone must never qualify a run. No input, output, key, token or tensor values
are added to these observations.

## API basis

The runtime-to-PCI and driver UUID operations use NVIDIA's documented
[CUDA runtime device API](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__DEVICE.html)
and [CUDA 12.5 driver device API](https://docs.nvidia.com/cuda/archive/12.5.0/cuda-driver-api/group__CUDA__DEVICE.html).
The CPU fallback option is present in the pinned
[ONNX Runtime 1.20.1 session configuration](https://github.com/microsoft/onnxruntime/blob/v1.20.1/include/onnxruntime/core/session/onnxruntime_session_options_config_keys.h).
These API definitions justify implementation calls, not local GPU availability
or execution success. The accepted Tiger subject remains one physical RTX GPU;
this change does not qualify MIG or multi-GPU execution.
