# T007 Selection grant binding checkpoint

**Date**: 2026-08-22  
**Status**: partial implementation; transport and Targeted cache gates remain open

## Implemented

- A streamed Selection grant still carries only the RSA-wrapped 32-byte event
  key as its secret payload.
- The grant `keyId` is now a deterministic pre-Selection binding digest.  It
  covers request ID, requester, service, selected Provider, Provider boot
  epoch, attempt, generation, stream epoch, event-key commitment, UserToken,
  policy epoch, and deadline.
- The grant uses the version-1 `HybridMessageEnvelope` encoding so the full
  32-byte binding digest is preserved. The ordinary version-2 compact key-id
  encoding is not used for this authorization binding.
- The final Selection digest is intentionally excluded from this grant digest
  because the grant is itself a field of the Selection message.  The Provider
  constructs the full binding after receiving Selection and checks both the
  pre-Selection grant digest and the final stream binding before creating its
  publisher.
- Missing or empty `providerBootEpoch`, recipient fields, malformed wrapped
  keys, commitment mismatch, and binding-digest mismatch fail closed.

## Verification

```text
./waf build --target=unit-tests -j2                         PASS
./build/unit-tests --run_test=Spec175InvocationStreamMessage,
  Spec175InvocationStreamLifecycle --report_level=no       25 cases PASS
```

The lifecycle suite includes `StreamGrantBindingExcludesOnlyFinalSelectionDigest`:
changing only the final plan digest leaves the pre-Selection digest unchanged,
while changing the selected Provider changes it.  Existing generic authorization,
Targeted token/replay, encrypted-permission, and message suites were rerun in
the preceding 154-case focused run; no regression was observed.

The real one-Provider Normal integration case now also verifies that the
Provider unwraps the grant and reaches the streaming handler.  The companion
Targeted case verifies a cache miss is represented by one
`TargetedBootstrapRequest`, followed by one ACK/Selection grant projection and
the same streamed event/End/Response closure; see
`evidence/t008-integration-20260822.md`.

## Remaining boundary

This checkpoint does not close T007.  Cached selection-free Targeted, selected
Provider-only multi-Provider delivery, token/replay/policy negatives, and
replacement creating a new grant/event-key epoch remain open. No Tiger or SIF
promotion is authorized by this evidence.
