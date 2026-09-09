# R10-B1 Native REPO_REF Preparation — 2026-09-09

## Scope and allocation

本批只处理 native requester 的 `REPO_REF` preparation 边界。生产入口为
`NativeInferenceClient::request` → `dispatchOperation` →
`NativeRequestPreparation::prepareInput` → `encodeNativeRequestEnvelope`；Provider
继续在 `NativeProviderHandler::initialInputsFromRequest` 的 fetch/decrypt 边界读取加密
大数据。requester 不解密引用，也不增加 Python planner fallback。

## Coverage matrix

| Lane | Evidence |
| --- | --- |
| `production entry/callers` | `NativeInferenceClient::request`, `dispatchOperation`, `NativeRequestPreparation::prepareInput`; maintained Qwen/YOLO facades remain inline callers |
| `implementation and wire` | `NativePreparedInput` now accepts either adapter-encoded INLINE bytes or a canonical encrypted reference; v2 envelope transport fields are unchanged |
| `test/harness/oracle` | `Spec182Preparation/RepositoryReferencePreparationPreservesEncryptedIdentity`; `Spec182ClientState/RepositoryReferenceReachesNativePreparationBoundary`; existing REPO_REF envelope assertions |
| `build/source closure` | `NativeRequestPreparation.cpp/.hpp`, `NativeInferenceClient.cpp`, the two unit test translation units, `tests/wscript` `unit-tests` target |
| `migration/evidence` | `APPClient.request_native_payload` and maintained callers are unchanged; real encrypted fetch, Provider two-turn behavior, and T016 remain open |

## Static review

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) against
the complete R10-B1 diff and the call paths above. The first pass found one compile coverage
gap: the new preparation test called `nativeCanonicalJson` without including
`NativeCanonicalJson.hpp`. The test now includes that header; this is recorded as the first
compile boundary below. No further control finding was identified in the revised diff.

## First-boundary failure and changed check

The first batch build from the repository root used:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/time -f 'ELAPSED=%E MAXRSS=%MKB' ./waf build --targets=unit-tests -j4
```

It stopped at `tests/unit-tests/di-native-preparation.t.cpp:437` with
`error: ‘nativeCanonicalJson’ was not declared in this scope`; exit code 1, elapsed 0:16.62,
maximum RSS 1,357,032 KB. The raw first output is preserved by the session command record;
the retry adds an explicit test-source include closure check before rebuilding. No runtime or
protocol verdict is inferred from this compile failure.

The changed include was then reviewed and the retry completed successfully:

```text
./waf build --targets=unit-tests -j4                         exit=0, 20.37s, max RSS=1,121,768 KB
unit-tests --run_test=Spec182Preparation/*                  exit=0, 18 cases
unit-tests --run_test=Spec182ClientState/*                  exit=0, 17 cases
unit-tests --run_test=Spec182PlanSealer/*                   exit=0, 12 cases
unit-tests --run_test=Spec182V3Placement/*                  exit=0, 9 cases
unit-tests (full)                                            exit=0, 1015 cases
```

The focused commands were run from the repository root so relative fixtures resolved from
`tests/fixtures/spec182`; raw retry logs are in `.codex-tmp/spec182-r10-b1/` and the first
compile boundary is in `.codex-tmp/spec182-r10-b1-build-r1/result.txt`. The incremental build
used the cached system-first compiler closure (`/usr/bin/g++ -B/usr/bin`); `vmstat 1` during the
retry showed no sustained swap-in/out or stall.

As a post-build regression check, the existing real-Provider conversation selector
`Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation*` also passed all three cases
(FULL_CONTEXT→receipt/control/commit→APPEND_DELTA, single-provider negative, and alternate
replacement). Its raw output is `.codex-tmp/spec182-r10-b1/r4b6.log`; this is additional
regression evidence and does not convert the R4-B6 or T016 qualification gaps into PASS.

## Batch retrospective (pending)

- Static review found: missing test header closure, fixed before retry; revised diff had no
  further actionable finding.
- Compile/link found: the missing declaration on the first attempt; retry build passed.
- Runtime/test found: four focused Spec182 selectors and the full 1015-case unit target passed.
- Still unobserved: requester-to-Provider encrypted fetch/decrypt, cross-process behavior,
  MiniNDN/T016 and qualification.

`Closure decision`: `CLOSED_FOR_VALIDATION` for the local C++ REPO_REF preparation boundary;
Provider/network, cross-process, MiniNDN/T016 and qualification remain open and this batch
cannot produce `QUALIFICATION_PASS`.
