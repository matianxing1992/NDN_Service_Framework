# R10-B11 Requester REPO_REF Provider Fetch Evidence

**Date**: 2026-09-09
**Batch**: R10-B11
**Baseline**: `9f80a1ce` (R10-B9 requester/Core-wire checkpoint)
**Scope**: the existing one-process R4-B6 real-Provider conversation fixture.  This batch adds
Provider consumption of the exact reference emitted by the configured native requester; it does
not claim cross-process or T016 qualification.

## Behavior boundary

The `Spec182R4B6RealProviderRepositoryReference` selector now parses the requester-produced v2
envelope at the Provider collaboration boundary, checks `input_transport=REPO_REF` and the exact
published data name, calls `CollaborationContext::fetchEncryptedLargeData`, and compares the
recovered plaintext with the native publisher's catalog bytes before emitting the normal
conversation stream and receipt/control/commit result. The existing inline selector remains a
regression case. R10-B5/B6 still provide independent malformed, missing-object, and size-mismatch
fail-closed coverage.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | configured `NativeInferenceClient::request`; R4-B6 real `ServiceProvider` collaboration handler | `rg -n "fetchEncryptedLargeData|Spec182R4B6RealProviderRepositoryReference" tests/integration-tests/ndnsf-di-core-flow.t.cpp` | One-process requester → Core → Provider fetch/decrypt → conversation result is observed |
| `implementation and wire` | `covered` | `NativeApplicationInput::RepositoryReference`; v2 reference fields; `CollaborationContext::fetchEncryptedLargeData` | exact data name and fetched plaintext assertions | Provider consumes the requester identity-bound reference before producing output; no Python decryption or fallback was added |
| `test/harness/oracle` | `covered` | `Spec170NdnsfDiCoreFlow` new selector plus inline regression | separate Boost.Test selectors | Recovered bytes are compared to the publisher's exact plaintext; no synthetic runner result substitutes for fetch |
| `build/source closure` | `covered` | existing `integration-tests` Waf target and `ndnsf-di-core-flow.t.cpp` | system-first `-j4` build, 118/118 | Waf completed in 35.626 s; `vmstat` showed no sustained swap-in/out after initial samples |
| `migration/evidence` | `covered` | R10-B5/B6, R10-B9, this record, task/plan links and failure-log | `git diff --check`; design validator | Local fetch boundary is closed; maintained caller cross-process execution, namespace/cleanup and T016 remain open |

## Review trace

- Official read-only skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- The review checked the request payload copy, data-name/service binding, fetch lifetime, failure
  propagation, selector registration and preservation of the inline baseline. No actionable issue
  remained.

## Validation

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4
  exit 0; 118/118; Waf 35.626s

./build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderRepositoryReference
  exit 0; one case; no errors detected

./build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation
  exit 0; one case; no errors detected
```

No network/Tiger/T016 run was performed.  The design validator remains the document gate and does
not report runtime qualification.

## Batch retrospective

- `static`: exact requester reference identity and Provider fetch/service scope are checked at the
  production collaboration boundary.
- `compile/link`: the existing integration target rebuilt successfully with system-first `-j4`;
  no new target or source closure was required.
- `runtime/test`: the Provider recovered the exact published plaintext before the normal
  conversation output; the inline path stayed green.
- `unobserved`: cross-process maintained caller execution, stream/conversation qualification,
  legacy zero-use, namespace/child/socket cleanup, and T016 remain open.
- `batch expansion`: limited to one stable `REPO_REF` input variant; negative transport cases stay
  in R10-B5/B6.

## Closure decision

`CLOSED_FOR_VALIDATION` for the bounded one-process requester-produced reference consumption
boundary. Parent Spec182 tasks and T016 remain `PARTIAL`/`UNQUALIFIED`.
