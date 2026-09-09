# R10-B9 Requester REPO_REF Core-Wire Evidence

**Date**: 2026-09-09
**Batch**: R10-B9
**Baseline**: `7b3f0e94` (R10-B8 cross-task audit status checkpoint)
**Scope**: one existing real-Provider `NativeInferenceClient` conversation fixture and its
requester wire oracle.  The batch does not change Provider decryption, external configuration, or
T016 qualification.

## Behavior boundary

`Spec182R4B6RealProviderConversation` already drives a configured native requester through the
real `ServiceProvider` ACK/Selection/stream/receipt/control/commit fixture.  This batch adds
`Spec182R4B6RealProviderRepositoryReference`, which publishes an encrypted catalog object through
the existing native publisher, binds its returned manifest metadata into a canonical
`NativeApplicationInput::RepositoryReference`, and runs the same two-turn conversation.  The
Provider ACK handler checks the actual request envelope for `input_transport=REPO_REF`, an empty
`input_payload_b64`, and the exact published data/manifest identity.  The fixture's collaboration
handler remains a deterministic conversation responder; Provider fetch/decrypt behavior is covered
separately by R10-B5/B6 and is not inferred here.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `NativeInferenceClient::request`; `runR4B6RealProviderConversationCase`; new `Spec182R4B6RealProviderRepositoryReference` | `rg -n "Spec182R4B6RealProviderRepositoryReference|RepositoryReference" tests/integration-tests/ndnsf-di-core-flow.t.cpp` | Maintained native requester now has a real Core-wire REPO_REF case; existing inline conversation selector remains a regression oracle |
| `implementation and wire` | `covered` | `NativeApplicationInput::RepositoryReference`; v2 envelope `input_transport`/`input_reference`; Provider ACK ingress | selector-side canonical JSON inspection and exact published data/manifest comparison | No requester-side decryption or alternate wire was added; the reference remains metadata-only |
| `test/harness/oracle` | `covered` | `Spec170NdnsfDiCoreFlow` registration and ACK oracle | separate baseline/new Boost.Test selectors; first oracle failure retained in failure-log | A temporary-buffer iterator defect was fixed by copying one payload buffer before parsing; no product defect assigned |
| `build/source closure` | `covered` | existing `integration-tests` Waf target and `ndnsf-di-core-flow.t.cpp` | system-first `-j4` build, 118/118 link; `vmstat` after its first line | Build finished in 35.882 s; no sustained swap-in/out observed |
| `migration/evidence` | `covered` | R10-B5/B6 Provider boundaries, R10-B8 audit, this evidence and failure-log entry | `git diff --check`; design validator; evidence links | Local requester/Core wire exit is explicit; Provider fetch qualification, maintained caller execution and T016 remain open |

## Review trace

- Official read-only skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- The review checked the complete test diff, payload ownership, canonical reference binding,
  selector registration, baseline preservation, and the distinction between Core-wire observation
  and Provider fetch/decrypt qualification.
- No actionable product finding remained after replacing the temporary-buffer iterator range.

## Validation

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j4
  exit 0; 118/118; Waf 35.882s

./build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation
  exit 0; one case; no errors detected

./build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderRepositoryReference
  exit 0; one case; no errors detected
```

`git diff --check` passes.  The final `vmstat 1 5` capture has no sustained swap activity after
the initial sample.  The comma-separated Boost.Test attempt is retained as a selector setup
boundary and is not used as evidence.  No network/Tiger/T016 run was performed.

## Batch retrospective

- `static`: the oracle was tightened to exact reference identity; the first temporary-iterator
  defect and its correction are recorded rather than counted as a protocol failure.
- `compile/link`: the existing integration target rebuilt successfully with the system-first
  `-j4` policy; no source registration outside the target changed.
- `runtime/test`: configured native requester → Core → real Provider conversation and the new
  REPO_REF envelope observation both pass; the fixture still supplies deterministic stream output.
- `unobserved`: Provider fetch/decrypt for this requester-produced reference, cross-process
  maintained caller execution, streaming/conversation qualification, legacy zero-use, and T016
  remain open.
- `batch expansion`: limited to one input-transport variant in the existing R4-B6 fixture and its
  evidence; no new runtime owner or Python fallback was introduced.

## Closure decision

`CLOSED_FOR_VALIDATION` for the bounded requester Core-wire REPO_REF boundary.  This evidence
strengthens the production-chain audit without promoting T010/T011/T013/T016 or Spec182 overall.
