# T008 one-Provider integration checkpoint

**Date**: 2026-08-22  
**Status**: partial implementation; the core Normal transport path is now
executed locally

## Verification

```text
./waf build --target=integration-tests -j2                         PASS
build/integration-tests --run_test=Spec175InvocationStream --log_level=message
  6 cases PASS
```

The passing cases use the reusable in-process fixture with real DummyClientFace
bridges, SVS publication, encrypted Request/ACK/Selection/Response messages,
one Provider-specific RSA-wrapped event-key grant, Provider event signing,
exact event Interests, ordered callbacks, End, and final Response closure.
They also verify that the service-only Normal overload does not take a Provider
list and that the invalid Targeted/Normal overload is rejected before publish.

The six cases are:

1. ordered events followed by End and one final Response;
2. the same service-only Normal path completing with zero events;
3. a Provider-side `writer.fail(ProviderFailure, ...)` delivered as one
   terminal streaming error; and
4. cancellation from the first application-event callback, fencing all later
   event/complete callbacks;
5. a Targeted cache miss represented by one `TargetedBootstrapRequest`, followed
   by one ACK/Selection and the streamed event/End/Response path; and
6. rejection of the invalid Normal/Targeted overload before publication.

The test exposed and fixed three transport defects: Normal ACK registration
for streaming-only services, pre-Selection grant validation of the intentionally
unset final plan digest, and insertion of a stack `ndn::Data` into the IMS.
Stream grants use the version-1 envelope form so their full SHA-256 binding
digest is not truncated by the ordinary V2 compact key-id encoding. LocalMock
recipient unwrap uses the fixture TPM only when explicitly bound by the test
helper; production providers retain their own key chain.

## Remaining boundary

This does not close T008. The fixture still needs loss/retry, capacity, and
full unary/Targeted regression cases. T007 still needs replacement epochs,
replay negatives, and selected-Provider-only multi-Provider evidence. No SIF
or Tiger promotion is authorized by this checkpoint.
