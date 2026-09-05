# Request-scoped inline-response tamper evidence

**Date**: 2026-09-03  
**Branch**: `UAV-Experimental`  
**Scope**: component-level response confidentiality regression

## Executed case

`tests/integration-tests/request-scoped-selection.t.cpp` now runs the shared
selected-provider flow in two modes:

- `SelectedProviderReceivesExactEncryptedInputOnly`: 9,000-byte response,
  compact large-response reference, Provider IMS fetch, ordered segment
  reconstruction, and Selection replay rejection.
- `ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery`: short response,
  inline request-scoped `AeadEnvelope`, one-byte ciphertext mutation, and
  untouched-response delivery after the tampered packet is rejected.
- `RevokedUserCannotReceiveResponseAfterProviderExecution`: installs a newer
  revoking ControllerVersion after Provider execution but before User delivery,
  then verifies one timeout and no application callback.

The tamper assertion is deliberately placed before the valid response handler
call. It verifies that authentication fails before application delivery and
before nonce reservation consumes the valid response attempt.

## Command and result

The source was compiled against the current tree with Clang and the repository
ndn-cxx/NDN-SVS/NAC-ABE objects. The focused executable was relinked with the
current `ServiceProvider.cpp` object (including the LocalMock ingress helper):

```text
/tmp/spec179-request-selection-20260903 \
  --run_test=RequestScopedSelection --log_level=message
```

Result: `Running 3 test cases ... *** No errors detected`.

The run emitted two deterministic bootstrap/request-terminal traces and exited
with status 0. This is component evidence only. Cross-user recipient checks,
configured trust-schema validation, response signature tampering, large-segment
tampering, Targeted/stream response paths, Controller revocation during an
  active large/Targeted/stream response, cross-user isolation, and the MiniNDN
  gate remain pending.
