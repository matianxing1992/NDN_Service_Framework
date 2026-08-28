# Post-Selection assignment/request integration gate (2026-08-17)

## Why the earlier tests missed the failure

The earlier Spec170 tests stopped at a synthetic boundary instead of running
the production state machine:

1. The request publication was decoded in the test and passed to
   `ServiceProvider::handleDecryptedRequestByName(...)`. That helper is an
   immediate execution API. It deliberately rejects `AllSelected` before a
   Selection exists, so it never creates the Provider pending-request entry
   that Selection needs.
2. ACKs were constructed and published by the test, rather than being emitted
   by `OnRequestDecryptionSuccessCallbackV2` and
   `finishAckDecisionOnEventLoop`. Thus token validation, ACK admission, and
   pending request/provider-token storage were not under test.
3. The old multi-Provider selection test stored an already-computed response
   in a map and published it when Selection was observed. It did not invoke
   `OnServiceSelectionMessageDecryptionSuccessCallbackV2`, assignment-set
   parsing, `dispatchCollaborationExecutionAsync`, or the collaboration
   handler.
4. Unit tests covered the User-side digest/selection bookkeeping and the
   Provider parser with synthetic wires, but did not connect those operations
   through an in-process SVS publication boundary.

Consequently, the Tiger D2a failure (ACK and Selection visible, no Provider
request/final Response) was outside the old test coverage. The first focused
attempt reproduced the missing gate as:

```text
AllSelected requires selection before execution for /Inference/NativeTracer
```

## Corrected local gates

The newly added production-ingress gates include
`Spec170NativePostSelection/ProductionIngressRunsNativePostSelectionAssignmentFetch`
and
`Spec170NativePostSelection/ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse`.
The narrower companion is
`Spec170NdnsfDiCoreFlow/ProductionIngressRunsRequestSelectionAssignmentDispatch`.
It attaches the LocalMock provider to the real in-process `SVSPubSub`, calls
`ServiceProvider::init()` so the production request/selection regexes are
registered, and publishes real V2 HybridMessageEnvelope request and Selection
packets. The receive key is seeded only to keep NAC-ABE bootstrap outside this
transport gate. The observed path is:

```text
SVS request publication
  -> ServiceProvider::OnRequest
  -> REQUEST_RECEIVED / Hybrid decrypt / REQUEST_DECRYPT_DONE
  -> ACK admission + pending request state
SVS Selection publication
  -> handleServiceSelectionMessage
  -> SELECTION_OBSERVED / Hybrid decrypt / SELECTION_DECRYPT_DONE
  -> assignment preparation
  -> collaboration handler
  -> EXECUTION_DONE
```

It passed as part of the expanded native matrix. The broader
`ProductionIngressRunsEndToEndSelectionAssignmentResponse` gate now drives a
four-envelope opaque assignment set through the same production subscriptions
and verifies the final User Response; it passed with **22 assertions**. The
fixture now uses numeric User and Provider SVS session suffixes, matching the
production node shape.

The four-Provider gate
`ProductionIngressRunsFourProviderRoleSplitRequestSelectionResponse` adds the
same production-ingress requirement to D0: all four Providers must receive the
Request, parse their own Selection assignment, dispatch their role handler, and
accept a selected Response. It passed with **41 assertions**.

The same production-ingress boundary is now exercised by the D2 gates:

* `ProductionIngressRunsD2aAssignmentIntoTwoDeviceRuntime`: **8/8**;
* `ProductionIngressRunsD2bSelectionIntoSvsDataV1`: **12/12**;
* `ProductionIngressRunsD2hFrozenHeterogeneousMappings`: **14/14**.

These cases deliberately start with a published Request and Selection rather
than calling post-decryption callbacks from the test. They therefore cover the
assignment/request handoff that was missing from the earlier tests.

`PreconfiguredEnvironmentRunsSameProviderMultiRoleCollaboration` now uses the
preconfigured MiniNDN-style fixture and performs this vertical flow:

```text
User RequestCollaboration (AllSelected)
  -> request publication through local SVS
  -> Provider OnRequestDecryptionSuccessCallbackV2
  -> ACK admission + pending request/provider token
  -> User ACK matching and four-role assignment construction
  -> Selection publication through local SVS
  -> Provider OnServiceSelectionMessageDecryptionSuccessCallbackV2
  -> opaque assignment parsing + collaboration handler
  -> Provider final Response publication through local SVS
  -> User response validation and callback
```

That test still exercises the complete four-role callback/response projection;
it asserts request receipt, Selection receipt, four parsed role assignments,
handler execution, final response, and no timeout. The complete integration
binary now passes the expanded native target:

```text
24 test cases passed
323 assertions passed
```

The three native post-Selection cases are in the dedicated
`Spec170NativePostSelection` suite and independently pass with 35 assertions.
This suite boundary is part of the gate: a broad Spec170 suite filter must not
silently omit these tests.

The production-ingress gate closes the local transport/decryption regression
for the post-Selection path. It does not by itself prove CUDA, but the local
pre-Tiger matrix now also contains
the D0 four-Provider role split, the D2a two-device scheduler/logical-group
case, the D2b native `NDNSF_DATA_V1` SVS segment cases, and both frozen D2h
profiles `[1,2,1]` and `[2,1,2]`. These are still in-process/CPU evidence;
Tiger D1/D2a/D2b/D2h hardware and process-isolation evidence remains required.

In addition, the deployment-faithful local gate
`tests/python/test_spec170_real_minindn_gate.py` now runs the real
`Experiments/NDNSF_DI_NativeTracer_Minindn.py` process topology with the
configured CPU ONNX Runtime ABI. Its `default` and `single-provider` cases
passed on 2026-08-17, each observing ACK matching, four role assignments,
Selection delivery, Provider execution, and a final User Response. This gate
does not use the synthetic deterministic runner; the latter remains a negative
runtime-readiness control and must not be counted as a successful production
execution.
