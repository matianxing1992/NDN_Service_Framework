# Spec179 full regression red/green record

Date: 2026-09-04 (CDT).  Build: `build-clang-spec179-nac3`
(Clang 10, NAC-ABE prefix `/tmp/nac-abe-spec179-exact-prefix`,
`LD_LIBRARY_PATH=/tmp/nac-abe-spec179-exact-prefix/lib`).
Binary state: current worktree HEAD `e2d793e8` plus today's spec179
semantic fixes (cross-service public-parameter identity for the
`abeGenerationChanged` decision and the identity-wide DKEY refresh wave
dedupe in `ServiceUser`/`ServiceProvider`).

## GREEN — spec179-focused integration suites (full suite runs, exit 0)

```text
ControllerRevocationFlow        (38 cases)
ControllerVersionRefresh
RequestScopedSelection
RequestScopedResponseConfidentiality  (4 cases)
Spec175InvocationStream
```

Also green in the same integration binary on 2026-09-04:
`NdnSvsSmoke`, `NdnsfDataV1SvsFlow`, `UavCollaborationFlow`,
`UavMultiViewIntegration` (all cases in these suites passed within the
full-binary run described below; the only failures were the three DI
cases listed under RED).

## GREEN — spec179-relevant unit suites (full suite runs, exit 0)

```text
ControllerRevocationState
ControllerRevocationPolicy
GenericDynamicApi
GenericDynamicApiLocalInvocation
GenericOpaqueSelection
RequestScopedConfidentiality
Spec175InvocationStreamLifecycle
Spec175InvocationStreamMessage
```

## RED — full-binary runs (unit and integration)

### Integration full run — 11 assertion errors in 3 pre-existing DI cases

`timeout 3000 ./build-clang-spec179-nac3/integration-tests --log_level=message`
(integration binary, 11 suites / 128 cases):  **11 failures**, all in
`Spec170*` DI production-collaboration cases:

1. `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2aAssignmentIntoTwoDeviceRuntime`
   — 6 assertion errors (`ndnsf-di-core-flow.t.cpp:3343-3348`):
   handler never called, assignmentRoleCount == 0, no runtime output.
2. `Spec170NativePostSelection/ProductionIngressRunsNativePostSelectionAssignmentFetch`
   — 3 assertion errors (`ndnsf-di-core-flow.t.cpp:804-807`).
3. `Spec170NativePostSelection/ProductionIngressReportsNativeDeviceMismatch`
   — 2 assertion errors (`ndnsf-di-core-flow.t.cpp:817-820`).

Classification: **pre-existing, schedule-dependent DI flakiness, out of
spec179 scope.** Evidence:

- The same 11 errors reproduce under a controlled legacy-vs-new
  experiment: a binary built from the six pre-spec179 semantic deltas
  reverted (legacy semantics) fails the same 3 cases with the same 11
  assertions (`/tmp/spec179-bisect-di*.log`, 2026-09-04).  The spec179
  changes therefore do not cause these failures.
- Isolated re-runs rotate which case fails per run and how many
  assertions fire (3 cases / 11 assertions in the full run vs 4 cases /
  14 errors in one isolated sweep).  `NativeDeviceMismatch` passed once
  in three runs (pass ≈ 1.2 s, fails ≈ 0.8 s) — timing sensitive, SVS
  scheduling dependent.  This matches the pre-existing recorded class
  "schedule-dependent flakiness in the DI suite (SVS timing)" and the
  09-03 "124-green" baseline where `D2a` passed.
- spec179 touches no DI runtime/coordination code touched by these
  cases (`NDNSF-DistributedInference/...` unchanged in the worktree).

### Unit full run — 1 SIGFPE in a pre-existing DI codec case

`timeout 3000 ./build-clang-spec179-nac3/unit-tests --log_level=message`
(686 cases):  **1 failure**:

```text
NativeTensorBundleCodecRoundTripsPilotDtypesDynamicShapesAndKvOutputs:
signal: integer divide by zero; address of failing instruction 0x014515c3
```

- Reproduces deterministically in isolation: 3/3 runs, exit 201.
- gdb backtrace: SIGFPE in `ndnsf::di::validateNamedTensor` (+483),
  called from `encodeTensorBundle`, from the test method.
- Root cause: `NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.cpp`
  overflow guard
  `if (elements > std::numeric_limits<std::uint64_t>::max() / value)`
  executes `max()/0` when a shape dimension is 0.  Commit
  `75a17ea6` (2026-07-12, "Wire bounded Qwen MiniNDN generation")
  loosened the validation from `dim <= 0 → throw` (positive dims
  required) to `dim < 0 → throw` (0 allowed as a dynamic-dimension
  marker with empty payload), but the overflow guard was not made
  zero-safe.  The test case pins the new contract (`past.1.key` with
  shape `{1,2,0,4}` and an empty payload round-trips).
- Classification: **pre-existing deterministic codec defect, committed
  2026-07-12 (two months before spec179 work), file untouched by the
  spec179 diff, out of spec179 scope** (DI codec domain).  No spec179
  normative matrix row depends on this case.  Recorded here as a
  residual risk for the DI owner; no spec179 code change was made to
  mask it.
