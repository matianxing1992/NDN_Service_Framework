# R11-B5 Native Recovery Process

**Date**: 2026-09-10
**Status**: CLOSED_FOR_VALIDATION (bounded Provider restart rejection; parent qualification remains open)
**Scope**: C++ `DI_NativeRequester` journal checkpoint, Provider process kill/restart, and safe continuation rejection

## Allocation and interruption point

This batch reuses the R11-B4 native process chain and selects one explicit interruption point:
after a successful `FULL_CONTEXT` turn has written the requester-owned checkpoint, the Provider
process is killed with SIGKILL before the next `APPEND_DELTA` turn. The restarted Provider uses
the same plan, manifest, identity, and artifact cache configuration but has no durable Provider
KV state. The expected safe behavior is explicit rejection; the requester journal is not treated
as proof that Provider KV state survived.

## Final process result

The first C++ requester process completed the stream oracle
`[4,5,6,7,8,9,10,2]`, `COMMIT`/`FINALIZE`, and wrote a persisted conversation checkpoint.
The original Provider exited with `-9` after SIGKILL. A fresh Provider process reached
`NDNSF_DI_NATIVE_PROVIDER_READY`, then rejected the resumed role with
`PROVIDER_CONVERSATION_STATE_MISSING`. The second requester returned nonzero with
`NATIVE_STREAM_FAILED`; it emitted neither `NATIVE_REQUEST_SUCCEEDED` nor a new checkpoint.
The harness also retained the wrong-parent negative from R11-B4, which returned
`NATIVE_CONVERSATION_BEGIN_FAILED` with `DI_NATIVE_CONVERSATION_PARENT_MISMATCH`.

The restarted Provider log contains no `NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` and no
`STREAM_EVENT_OBSERVED`, so it did not execute or publish a duplicate prefix after restart.
The driver asserts all positive/negative markers and the absence of restart execution markers.

Raw run: `/tmp/spec182-r11-b5-recovery-checked/` (`requester.log`, `requester-second.log`,
`requester-wrong-parent.log`, `provider.log`, `provider-restart.log`). The command was:

```text
python3 tests/standalone/run-spec182-native-stream-process.py --conversation --recovery \
  --run-root /tmp/spec182-r11-b5-recovery-checked
```

The driver exited `0`; its recorded process results are `requester rc 0`, `provider first rc -9`,
`requester-second rc 1`, and `requester-wrong-parent rc 1`. The C++ build and selectors used for
the preceding R11-B4 checkpoint remain valid: native targets built with system-first `-j2`,
`unit-tests --run_test='Spec182*'` passed 256/256 cases and 7077/7077 assertions, and
`integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*'` passed 9/9 cases and
55/55 assertions. The recovery run itself changed only the process harness and exercised the
already-built C++ request/provider chain.

This closes R11-B5's bounded restart/rejection evidence. It does not claim durable Provider KV
recovery, replacement, cleanup, maintained caller migration, no-Python operation, or whole-Spec
qualification; those remain R11-B6--B9 and T010--T017 work.
