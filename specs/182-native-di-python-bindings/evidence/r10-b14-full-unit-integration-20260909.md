# R10-B14 Same-Source Unit and Integration Validation

**Date**: 2026-09-09
**Batch**: R10-B14
**Baseline**: `298220b9` (R10-B13 recipe-oracle repair)
**Scope**: complete local C++ unit and integration suites from `build-nac182`.

## Validation

The complete unit suite passed after the R10-B13 source repair:

```text
./build-nac182/unit-tests --log_level=test_suite
exit 0; no errors detected; elapsed 2:16.88; maximum RSS 8,396,612 KB
```

The complete integration suite then passed from the same build closure:

```text
./build-nac182/integration-tests --log_level=test_suite
exit 0; no errors detected; elapsed 3:15.30
```

The full integration raw log and `vmstat 1 8` capture are retained at
`.codex-tmp/spec182-t016-r4/`. The second run includes all four R10-B13
`Spec175NativeAssembly` cases that failed at the first stale-oracle boundary. During the
integration run, `vmstat` showed no swap-out after the initial sample; the preceding full unit
run did cause high swap-in and its resource observation is retained for the build-policy record.

## Five-lane review

| Lane | Status | Evidence / boundary |
| --- | --- | --- |
| `production entry/callers` | covered | frozen Waf unit/integration registrations and native production paths are exercised |
| `implementation and wire` | covered | DI/Core/ONNX/Provider integration cases are all green |
| `test/harness/oracle` | covered | complete suites, including repaired assembly fixture, report no errors |
| `build/source closure` | covered | `build-nac182`, system-first compiler path, Waf target closure; R10-B13 used `-j2` after swap-in |
| `migration/evidence` | partial | MiniNDN node/netns owner context, maintained caller cross-process run, no-Python trace and T016 remain open |

Read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`; no new actionable finding.

## Closure decision

`CLOSED_FOR_VALIDATION` for complete local unit/integration validation. This is not
`QUALIFICATION_PASS`: T015 convergence, MiniNDN/no-Python and maintained-caller cross-process
evidence still belong to T016.
