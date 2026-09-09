# R6-B7 Legacy D2b Compatibility Regression Boundary

**Date**: 2026-09-08
**Scope**: diagnostic only; old Spec170 `DATA_V1` compatibility path exercised by the
current Spec182-linked integration binary
**Status**: `PARTIAL`; no product task is closed by this record

## Observable boundary

The current `build-nac182/integration-tests` binary fails
`Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1` while the
older `build-system-j2/integration-tests` binary passed the same selector. In the
current run:

- both Providers receive and publish authenticated ACKs;
- the User receives both ACKs, closes the collaboration, commits the plan, and
  invokes `PublishServiceSelectionMessageV2` once for provider1 and once for
  provider0;
- only provider0 records `SELECTION_RECEIVED`, queues its handler, and reaches
  `EXECUTION_DONE`;
- provider1 never records a Selection callback, so `provider0Published` and
  `provider1HandlerCalled` remain false and the DATA_V1 fetch does not become
  ready before its bounded deadline.

The first failed boundary is therefore after User Selection publication and before
provider1's selection callback. This is not evidence that the Spec182 native
requester or protocol qualification passed or failed.

## Reproduction

```text
./build-nac182/integration-tests --run_test='Spec170NdnsfDiCoreFlow/*' --log_level=message
NDN_LOG='ndn_service_framework.ServiceUser=TRACE:ndn_service_framework.ServiceProvider=TRACE' \
  ./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1' \
  --log_level=message
NDNSF_HANDLER_THREADS=0 NDN_LOG='ndn_service_framework.ServiceUser=TRACE:ndn_service_framework.ServiceProvider=TRACE' \
  ./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1' \
  --log_level=message
```

The handler-thread override did not change the failure. The older binary passed
the isolated selector, so the boundary remains a current-source compatibility
regression rather than a declared fixture expectation.

## Coverage matrix

| Lane | Result and evidence |
| --- | --- |
| `production entry/callers` | `ServiceUser::CommitCollaborationPlan` → `evaluateAckSelection` → `PublishServiceSelectionMessageV2`; legacy D2b integration caller in `tests/integration-tests/ndnsf-di-core-flow.t.cpp` |
| `implementation and wire` | `ServiceUser.cpp` Selection publication and `ServiceProvider::handleServiceSelectionMessage`/`isFresh`; trace shows provider0 callback only |
| `test/harness/oracle` | Isolated D2b selector plus unfiltered `Spec170NdnsfDiCoreFlow/*`; provider0/provider1 flags and DATA_V1 fetch are independent assertions |
| `build/source closure` | Current `build-nac182` binary versus older `build-system-j2` binary; no source fix was retained; an exploratory full `-j4` rebuild was stopped after sustained swap-in/out and is not validation evidence |
| `migration/evidence` | Raw logs are preserved under `.codex-tmp/spec182-r6-b7-legacy-d2b-20260908/`; T013-B legacy zero-use and T016 qualification remain open |

## Miss classification and decision

- `Static findings`: no new source finding was made; the existing static gate did
  not identify this runtime ordering/receipt boundary.
- `Compile/build misses`: the current source requires a rebuild to compare against
  the older passing binary; no compile error was observed.
- `Runtime/test misses`: the isolated selector exposed provider1's missing
  Selection callback and bounded DATA_V1 timeout.
- `Unobserved`: exact SVS delivery/newness ordering at provider1 still needs a
  focused source probe or a safe compatibility fix.

`Closure decision`: `OPEN_FOR_NEXT_BATCH`. Do not mark T013-B, T016, or any
Spec182 qualification gate complete. A future repair must first prove the exact
provider1 receipt/newness boundary and add a named regression selector before
changing `ServiceProvider::isFresh` or the shared publication path.

## Review trace

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`)
against the diagnostic evidence scope and current `ServiceUser`/`ServiceProvider`
call paths. No product source change was retained; the result is a preserved
runtime boundary, not `STATIC_PASS` or `QUALIFICATION_PASS`.
