# Spec 112 Completion Summary

## Final Verdict

All five email-reported defects are closed for the current rebuilt local stack.
The final integrated candidate is `spec112-62b57fe47b2e3537ad23`; all six
declared MiniNDN cells are `SUCCESS`, the 300-exit OpenABE lifecycle gate passes,
and all focused native/Python suites pass.

This is not a claim that the reporter's older aarch64/NixOS commit combination
was rebuilt locally. The retained evidence applies to the recorded current
x86-64 Ubuntu, ndn-cxx 0.9.0, Boost 1.71, rebuilt ndn-svs, NAC-ABE, and Python
extension identities.

## Five-Defect Disposition

| Email defect | Current reproduction/root cause | Disposition | Focused evidence | Final evidence | Residual limit |
|---|---|---|---|---|---|
| 1. Segmented responses fail and poison Provider | Reproduced: pre-fix boundary matrix delivered 16/24; all four Providers exited when an 8917-B outer Data exceeded 8800 B. ndn-svs used fixed inner content sizing and exposed incomplete/failing publication state. | Iterative final signed-inner/signed-outer sizing; full logical preparation/storage before advertisement; rollback and lifetime-safe receive cleanup. | `us1-focused-validation.md`; `TestSVSPubSub` 16/16. | Four boundary cells: 24/24 byte-exact; burst: 102/102 in one Provider epoch. | 0% MiniNDN only; no Wi-Fi/loss characterization. |
| 2. Oversized response SIGABRT | Reproduced together with defect 1 at final outer wire size 8917 B. | Same lowest-owner ndn-svs fix contains encode/sign/store/Face errors and prevents oversize final packets. | Exact/below/above 8800-B unit boundary and injected failure tests. | Zero oversize-exception, SIGABRT, or SIGSEGV markers; all 8/16-KB forced-inline responses completed. | Network logs prove no observed abort; exact packet maxima are unit-level evidence. |
| 3. Python Targeted unusable | Reporter used an older binding. Current code already forces tokens on for Provider/User and registers `NormalAndTargeted`; no public token-disable exists. | No product-source change for this defect. Added real-binding positive/negative evidence. | `us2-python-targeted.md`; one handler, fail-closed token/replay cases. | Earlier immutable real-binding MiniNDN cells passed Normal and Targeted exactly once. | Old fork was not cherry-picked or retested. |
| 4. Targeted ignores `timeout_ms` | Reproduced in current code: the timer began only after publication returned, so admission/publication could wait outside the deadline. | Absolute Targeted deadline starts at pending-call creation; no Targeted grace extension; timer/request/queue cleanup and one terminal callback; Python sync uses copied inputs plus shared terminal state across its `+500 ms` local fallback, and async uses atomic arbitration. | C++ Targeted suite 18/18; `us3-targeted-timeout.md`; T048 convergence regression. | Final degraded async Targeted timeout: 1064.569 ms versus 1500-ms acceptance limit, 1 timeout, 0 response callbacks. | 500 ms is a harness allowance, not native deadline extension. |
| 5. NAC-ABE/OpenABE exit crash | Current dirty NAC-ABE already had one process-wide OpenABE thread and no destructor-time shutdown; evidence was missing. | Decision baseline passed, so no additional product-source edit. Added initialized role probe and immutable 100-cycle driver. | 10-cycle baseline 30/30; one-cycle schema test. | 100 Controller + 100 Provider + 100 User exits: 300/300; 0 SIGSEGV, 0 SIGABRT, 0 timeout. | Build was not ASan/UBSan-instrumented; marker scan is recorded, not claimed as sanitizer coverage. |

## Final Integrated Candidate

- Candidate:
  `results/spec112-segmented/spec112-62b57fe47b2e3537ad23/`
- Manifest SHA-256:
  `fceb07475a435081dee7907c78ac3bbcdbd33ebe3dc1cc60374a3884ab92f2b8`
- Campaign summary SHA-256:
  `d0126c5fe68da2aca654bc8b0c1fe793c258c6325985ece019f6e964837da6b5`
- Campaign CSV SHA-256:
  `01c4deec5025cba758644f0bd5eacb12e341bf0da38b1e1f8f8d334572d0f676`

The candidate identity was revalidated against the live repositories and all
bound binaries immediately after all six cells completed and before closeout
documentation changed the working diff.

| Final cell | Accepted observation |
|---|---|
| `final-boundary-async-normal` | 6/6 byte-exact |
| `final-boundary-async-targeted` | 6/6 byte-exact |
| `final-boundary-sync-normal` | 6/6 byte-exact |
| `final-boundary-sync-targeted` | 6/6 byte-exact |
| `final-burst-async-normal` | 80/80 8-KB + 10/10 64-B + 12/12 4-KB; restart count 0 |
| `final-timeout-async-targeted` | established request succeeds; degraded request times out once in 1064.569 ms |

All cells recorded 0% topology, exclusive ownership, forced
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1`, no reference markers, no wall
or disk stop, and immutable direct-child paths.

The earlier successful candidate `spec112-8d342a1f43c09a1b2b34` remains
immutable. A pre-completion audit found that its synchronous Python adapter
could retain stack-reference callbacks after the local fallback. T048 replaced
those captures with copied inputs and shared terminal state, rebuilt the
extension as SHA-256
`a9e4423eb85104a4d2061f278d09115bc83c1dfeccf3848357a3aa5865470eb5`,
and required this new final candidate rather than overwriting prior evidence.

## Preserved Negative/Diagnostic Evidence

- Pre-fix four-cell result remains the defect reproduction: 16/24 byte-exact
  and four Provider exits on the 8917-B packet.
- `spec112-8e46...` remains the earlier harness-classification failure; it was
  not rewritten as a pass.
- `spec112-0e2e...` and `spec112-89961...` retain lock-permission and missing-sudo
  timeout-harness startup failures. Neither entered a valid final experiment,
  and neither was overwritten or rerun.

## Scope

Spec 112 added no public checked-publish API, wire namespace, large-object
redesign, 5% loss experiment, DI/Docker/iTiger/UAV work, or tokens-off mode.
Automatic large-response reference behavior remains outside the forced-inline
diagnostic cells and its implementation was not redesigned.
