# B2 Durable Outcome Evidence

**Status**: DYNAMIC_PASS / focused B2 validation; formal qualification pending
**Spec**: `184-native-di-closure`
**Batch**: B2 / T003
**Production change**: `NativeInferenceClient::markTerminal` fences cancel/deadline/failure after
durable conversation publication; `NativeConversationCoordinator::commitTurn` exposes an
owner-side observation point after journal publication and before best-effort Provider `FINALIZE`.
The hook is optional, runs without the coordinator lock, and is used only to make the race
deterministic in the C++ fixture.

## Dynamic gate card

- Risk class: `lifetime/linearization`.
- Profile: `asan-ubsan`, independent tree `build-spec184-b2-asan`; normal comparison tree
  `build-spec184-b2-normal`.
- C++ selector: `Spec170NdnsfDiCoreFlow/Spec184DurableOutcome`.
- Parameters/boundaries: publish before cancel, deadline/terminal callback interleaving,
  delayed `FINALIZE`, authenticated checkpoint readback.
- Invariants: one successful handle outcome after durable publish; checkpoint/journal identity
  matches the coordinator record; no terminal downgrade; coordinator/request residue drains.
- Budget: one normal run plus two sanitizer repeats; bounded 180-second timeout per run.
- Toolchain/source identity: `/usr/bin/g++` with system-first PATH, Boost 1.71, explicit
  NDN-SVS/NAC-ABE/ONNX prefixes; binary digests recorded below.

## Static and behavior lanes

The static review covered `markTerminal`, `commitConversationTurn`,
`NativeConversationCoordinator::commitTurn`, the optional callback ownership, and the real
integration fixture/selector. The normal C++ selector passed after the test fixture was changed
to observe the coordinator's post-publish linearization point rather than attempting to infer
Provider `FINALIZE` from retained `waitFor()` records.

| Lane | Result | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `NativeInferenceClient::markTerminal`, `commitConversationTurn` |
| implementation and wire | covered | `NativeConversationCoordinator::commitTurn`, journal publish and finalize boundary |
| test/harness/oracle | covered | `tests/integration-tests/ndnsf-di-core-flow.t.cpp` / `Spec184DurableOutcome` |
| build/source closure | covered | `integration-tests` target; normal and sanitizer trees; `sha256sum` below |
| migration/evidence | N/A (B2 local behavior; caller migration is B4) | recorded as remaining |

Normal build and behavior:

```text
WAFLOCK=.lock-spec184-b2 ... ./waf -o build-spec184-b2-normal build --targets=integration-tests -j4 -v
exit=0; ELAPSED=0:36.33; MAXRSS=2093548KB
log=.codex-tmp/spec184-b2/normal-build-rerun5.log
selector log=.codex-tmp/spec184-b2/normal-test-rerun5.log; exit=0; *** No errors detected
```

## Sanitizer boundary

The first strict sanitizer run was retained at `.codex-tmp/spec184-b2/asan-test.log`. It stopped
at test teardown with `AddressSanitizer: new-delete-type-mismatch` while deleting an
`ndn::svs::SVSPubSub` allocated by the fixture. The stack is entirely in the external
`ndn-svs` object/layout boundary (`NdnsfIntegrationEnvironment::~NdnsfIntegrationEnvironment`);
the Spec184 production symbols were not reported. This was an external dependency ABI failure,
not a protocol result. The first failure remains preserved as the changed-gate input.

The independent sanitizer tree was reconfigured with the system-first compiler/linker and
completed 119 actions:

```text
log=.codex-tmp/spec184-b2/asan-build-nosized-tests.log
exit=0; ELAPSED=5:32.13; MAXRSS=3067364KB
```

Before accepting a sanitizer result, the NDN-SVS dependency was rebuilt from the current source
header/library pair with the system-first toolchain (`/home/tianxing/NDN/ndn-svs/.codex-tmp/
spec184-svs-build-20260911.log`, `-j4`, `ELAPSED=0:13.02`, `MAXRSS=940988KB`). The resulting
library digest is `12fea93c9b07fee000abb412746e4d170955ec270a51a52579d3851f47c8f1c8`. The
independent sanitizer tree was relinked against this ABI-consistent library
(`.codex-tmp/spec184-b2/asan-build-relink-svs.log`, exit 0).

The clean strict sanitizer selector then passed three times (`asan-test-svs-rebuilt.log`,
`asan-test-svs-repeat1.log`, `asan-test-svs-repeat2.log`), each bounded by 180 seconds, with no
ASan/UBSan report and `*** No errors detected`; this is the B2 `DYNAMIC_PASS` result. Diagnostic
reruns with the explicitly documented external-only option
`ASAN_OPTIONS=...:new_delete_type_mismatch=0` exited 0 twice (`asan-test-repeat1.log`,
`asan-test-repeat2.log`) and emitted no other ASan/UBSan report. They remain diagnostic only and
are not used for the pass.

```text
normal integration-tests sha256 25df91d2cb2d1f4a486db637194b0b39bf999bea63cd81c78476d23b461394cf
asan integration-tests sha256 3acfebcde27d6121c7b78675a8a040969ad207bf69de71a89cf21dfc3abd7da9
```

## Earlier fixture miss and changed gate

The first race fixture attempted to block Provider `FINALIZE` by observing retained
`waitFor()` records. That did not enter the intended boundary because `waitFor()` returns all
retained records and the handler returned on `COMMIT`; raw attempts are
`.codex-tmp/spec184-b2/normal-test-rerun.log`, `normal-test-rerun2.log`, and
`normal-test-rerun3.log`. The changed gate is the production coordinator
`afterDurableCommit` observation hook plus atomic entry/release flags in the C++ fixture. The
post-change normal selector and the rebuilt unsuppressed sanitizer selectors pass; the original
fixture and ABI failures remain separately classified above.

## Remaining

T003 is now closed for validation with focused C++ behavior and `asan-ubsan` `DYNAMIC_PASS`;
formal qualification remains pending. T004 checkpoint export, T005 caller convergence, T006
qualification matrix/convergence, T007 local qualification, and T008 handoff remain unstarted.
No MiniNDN, Qwen, no-Python, or external Tiger qualification is claimed by this record.
