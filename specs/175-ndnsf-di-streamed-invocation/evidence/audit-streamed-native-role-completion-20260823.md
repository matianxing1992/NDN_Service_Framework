# Spec175 native streamed multi-role audit evidence

**Date:** 2026-08-23  
**Scope:** development prerequisite only; not a G2 I-case manifest

## Subject

`Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse`
now drives one deferred collaboration Request through the real Request,
ACK-closed plan commit, encrypted per-Provider Selection, native collaboration
handlers, one intermediate role, one terminal role, streamed Event/End, and one
terminal Response. The default runner is the deterministic native integration
runner; it is not the Qwen/tiny-ONNX qualification subject and is not registered
as I01-I03 or I15.

## Commands and results

```text
./waf build --target=integration-tests -j2
  PASS; integration-tests linked successfully

build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse \
  --log_level=all
  PASS; no errors detected

build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse \
  --log_level=test_suite
  PASS; no errors detected

build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRejectTamperedD2bCapability \
  --log_level=test_suite
  PASS; no errors detected

build/integration-tests --log_level=message
  PASS; 52 test cases, no errors detected

build/unit-tests --log_level=message
  PASS; 559 test cases, no errors detected
```

## Observed streamed invariants

- one request ID and one committed collaboration plan;
- both selected Providers entered with `context.isStreamed() == true`;
- the non-terminal Provider completed with `collaboration role complete; no terminal response`;
- the terminal Provider published exactly one Response;
- one ordered application event and one validated End completed the user stream;
- the final result matched `detections0:features:d2b-native-payload`;
- the tampered capability case still rejected without a final response.

## Corrections made

1. `CollaborationRoleSpec::terminalResponseOwner` is included in the deferred
   plan digest and streamed plans require exactly one owner.
2. Only the terminal role initializes the user-side streamed event consumer;
   non-terminal selected Providers still receive Selection but do not create a
   second consumer for the same request.
3. The streamed test supplies opaque V3 JSON projections to the participant
   selector. `CommitCollaborationPlan` remains responsible for adding the
   framework assignment envelope; nested envelopes are not accepted as a test
   shortcut.
4. The production native handler calls `completeRole()` when its role has no
   terminal payload, releasing provider-side pending state without publishing a
   user-facing Response.

## Boundary

This evidence closes the previously missing development proof for the
non-final-role lifecycle and the streamed terminal-owner wiring. It does not
close T013-T019, formal G2, SIF/MiniNDN gates, or any TigerCluster claim:

- the default runner is deterministic and does not execute the committed tiny
  ONNX model;
- formal I01-I15 registration remains empty and the fail-closed G2 runner must
  continue to report missing cases;
- retention-expiry, formal 2-role ONNX fault cases, replacement, Python native
  transport/lifetime, and G3-G7 remain open.
