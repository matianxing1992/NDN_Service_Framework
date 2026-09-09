# R10-B22 Native DI Business Case

**Status**: PARTIAL / `STATIC_PASS` + direct selector `FOCUSED_BEHAVIOR_PASS`; owner/runner remains `UNQUALIFIED`  
**Date**: 2026-09-09  
**Baseline**: `dabce49acf8762a85f8a7470485c787dab0f2c36` (marker source change reviewed below)  
**Owner**: T014/T016 local execution boundary

## Scope and stable exit

This batch connects the existing C++ `Spec182R4B6RealProviderConversation` selector to the
canonical MiniNDN owner/runner. The selector constructs a protected `NativeRequestRuntime`, submits
two native requests through `NativeInferenceClient`, serves them through the production
`ServiceProvider` ingress, and asserts both native results. The stable exit is an isolated native
DI request/result observation with a marker emitted only after the second result assertion.

This does not claim true multi-process requester/provider transport, maintained caller migration,
I02-I08/PO completion, or T016 qualification. Those acceptance dependencies remain open.

## Minimum Review Record

Review path: `/home/tianxing/.codex/skills/review-agent/SKILL.md`  
Review SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`  
Review baseline: `dabce49acf8762a85f8a7470485c787dab0f2c36`  
Diff scope: `tests/integration-tests/ndnsf-di-core-flow.t.cpp`, this evidence, and the R10-B22
registration in `tasks.md`/`plan.md`.

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | covered | `runR4B6RealProviderConversationCase`; `Spec182R4B6RealProviderConversation` | CodeGraph query for `NativeInferenceClient request`, `rg` selector registration | Marker is on the existing successful two-turn path; replacement cases retain their own early return. |
| `implementation and wire` | covered | `NativeRequestRuntime`, `NativeInferenceClient::request`, `ServiceProvider` ingress in the same fixture | CodeGraph flow plus source read around fixture construction and result assertions | Marker is emitted after `BOOST_REQUIRE(second.status() == Succeeded)` and payload assertion. No production protocol change. |
| `test/harness/oracle` | covered | `integration-tests` selector; runner `businessOracle.stdoutMarker` | `rg` selector/target registration; runner manifest validation and marker evaluator tests | Marker is independent of runner evidence and cannot by itself promote qualification. |
| `build/source closure` | covered | `tests/wscript` `integration-tests` target; current source file | `rg` target registration; `/usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4` | Build returned rc=0 in 37.778s, but Waf used the existing `.codex-tmp/spec182-r4-b2/build` output; that actual binary is the one used below. |
| `migration/evidence` | gap | owner/runner handoff and maintained callers | planned fresh owner run with `PO-001` manifest; caller audit remains open | In-process fixture inside an isolated namespace is not multi-process transport evidence; preserve as partial. |

Static review found no introduced defect in the marker placement or selector registration. The
`migration/evidence` gap is intentional and is the closure boundary for this batch, not a reason to
pretend T016 is complete.

## Validation record

| Field | Result |
| --- | --- |
| `Static findings` | none; five lanes above reviewed, migration gap retained |
| `Compile/link misses` | manifest v1/v2 exposed target mismatch and missing ELF interpreter; v4 still exposed minimal-root loader search path before the process could run. All first boundaries are preserved in `.codex-tmp/spec182-r10-b22-native-di-owner{,-v2,-v3,-v4}`. |
| `Runtime/test misses` | direct selector passed and emitted `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` in 6.801s; owner/runner v4 reached the test process but failed at setup because `examples/trust-any.conf` was absent (`returncode=201`, first runtime boundary). |
| `Unobserved` | true multi-process transport, maintained callers, I02-I08 and T016 qualification; owner/runner data/config working-directory support is a separate next batch. |
| `Build measurement` | `/usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4`, rc=0, 37.778s; `vmstat` showed no sustained swap-out. Actual output: `.codex-tmp/spec182-r4-b2/build/integration-tests`. |
| `Behavior result` | `STATIC_PASS`; `BUILD_PASS`; direct C++ selector `FOCUSED_BEHAVIOR_PASS`; owner/runner `UNQUALIFIED`; not `QUALIFICATION_PASS`. |
| `Evidence / remaining` | direct log `.codex-tmp/spec182-r10-b22-direct-selector-20260909.log`; owner logs/trace in v1-v4 directories. Next batch must provide runner working-directory/config binding and rerun with a fresh output. |

## Batch Retrospective

- `static`: marker placement, selector registration, runner oracle contract and build target were
  checked; no control finding.
- `compile/link`: manifest v1 failed preflight (`process executable is undeclared`); v2 failed
  `execve` with missing `/lib64/ld-linux-x86-64.so.2`; v3 failed loader lookup for
  `libndn-cxx.so.0.9.0`. The corrected `/lib/<SONAME>` closure reached the test process in v4,
  so these are preserved as closure misses rather than hidden retries.
- `runtime/test`: the direct selector passed, while owner/runner v4 failed at the first fixture
  setup boundary because the relative trust configuration was not present in the minimal root.
- `unobserved`: multi-process transport, maintained callers, negative cases and final qualification
  remain outside this boundary.
- Batch expansion: none after the stable exit was registered; negative and caller cases belong to
  later batches with different selectors or acceptance exits.

## Closure decision

`OPEN_FOR_NEXT_BATCH`: the native source/build and direct selector are closed for this bounded
business case, but the owner/runner composition remains unqualified at its first fixture setup
boundary. A separate runner data/config working-directory batch must repair that boundary and use a
new output directory; this batch stays PARTIAL and does not promote T016.
