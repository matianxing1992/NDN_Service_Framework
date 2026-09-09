# R7-B1 R4-B6 Single-Provider Replacement Boundary

## Status and closure

`CLOSED_FOR_VALIDATION` for the CC-4c single-provider replacement negative. This batch
does not claim successful alternate-provider recovery, cross-process qualification, or
completion of T011-C/T016. The real Provider path now records the first recovery boundary
instead of treating a failed-provider/no-admitted-provider result as an implementation
failure.

## Scope and coverage

| Lane | Result | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `runR4B6RealProviderConversationCase(true)` calls public `NativeInferenceClient::request` and a registered `ServiceProvider` collaboration handler in `tests/integration-tests/ndnsf-di-core-flow.t.cpp` |
| implementation and wire | covered | Provider failure is injected through `CollaborationContext::failStream(ProviderFailure, ...)`; the next ACK is planned through the native replacement path and excludes the failed Provider |
| test/harness/oracle | covered for the bounded negative | `Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationReplacement` asserts `NATIVE_REQUEST_STAGE_FAILED`, the `DI_NATIVE_NO_ADMITTED_PROVIDER` cause, `ACK_CLOSED` boundary, one Provider invocation, and no conversation checkpoint |
| build/source closure | covered | Existing Waf integration target rebuilt with the changed test source; no manual source list was added. System-first toolchain was used with `-j2` after the prior swap-heavy exploratory build |
| migration/evidence | partial | Raw build, vmstat, failed expectation, final negative run, and positive regression are retained below; T011-C cross-process recovery and T016 remain open |

## Static review trace

The changed helper, Provider failure path, replacement options, negative assertions, test
registration, and existing integration target were read against the official
`/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`). Review baseline was
`c2c9746c`; `git diff --check` passed and no actionable static finding remained. The review
also checked that the negative branch does not read a missing record before checking status,
does not publish a successor checkpoint, and leaves the existing positive helper unchanged.

## First runtime boundary and correction

The first attempt expected a successful replacement from a one-Provider fixture. It built
successfully but exited 201 at the success assertion. The raw output records:

```text
R4-B6 first failed code=NATIVE_REQUEST_STAGE_FAILED domain=runtime
boundary=ACK_CLOSED message=native request stage failed: DI_NATIVE_NO_ADMITTED_PROVIDER
ackCalls=2 collaborationCalls=1
```

The first boundary is recovery ACK planning after the injected Provider failure. The failed
Provider is intentionally excluded from the recovery admission set, so the one-Provider
fixture has no admitted replacement. This is the contract's allowed single-provider negative,
not evidence that the requester should retry the same Provider or publish a partial parent.
The original expectation failure is preserved at
`.codex-tmp/spec182-r7-b1-r4b6-replacement-20260908/replacement.log` with
`replacement.rc=201`.

The test was then changed to assert the negative contract. The final run passed one case with
no errors and observed exactly one Provider collaboration invocation, two ACK calls, no
`NativeConversationCoordinator` record, and no successor checkpoint. A successful
alternate-provider replacement requires a separate multi-Provider harness and remains open.

## Batch validation

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=integration-tests -j2
  exit=0; Waf reported 36.791s; 118/118 tasks reached the integration link
./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationReplacement' \
  --log_level=message
  exit=0; 1 case, no errors
./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation' \
  --log_level=message
  exit=0; 1 case, no errors
```

Raw final logs and return codes are in
`.codex-tmp/spec182-r7-b1-r4b6-replacement-final-20260908/`. Its `vmstat.log` shows the
pre-existing swap allocation (`swpd` about 2.94 GiB) but no sustained `si`/`so` during this
short `-j2` rebuild. The host policy remains `-j4` by default; this invocation stayed at
`-j2` because the immediately preceding full exploratory build had already demonstrated
swap pressure.

## Task boundary

This closes the real single-provider negative member of R4-B6/CC-4c. R4-B6, T011-C, T012,
T013 and T016 remain `PARTIAL`: no successful alternate-provider recovery, cross-process
conversation, maintained-caller parity, or no-Python qualification was observed. The next
production batch must either configure a second admitted Provider for a positive replacement
or move to the next stable caller/owner dependency without changing the negative contract.
