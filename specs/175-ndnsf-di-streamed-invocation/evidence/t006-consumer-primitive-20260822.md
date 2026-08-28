# T006 consumer primitive checkpoint

**Status**: `partial` (standalone core only; task remains open)
**Date**: 2026-08-23

The standalone `StreamEventConsumer` exercises the bounded consumer-side
contract in the native lifecycle suite. It verifies the signed exact event
name and binding before decrypting, buffers a future cursor, requests one gap
retry, drains in order after the missing event arrives, suppresses a duplicate,
transitions to `Draining` on End, and accepts a matching final
`ResponseMessage` only after End. A Response arriving before End is retained
and completes only when the matching End is later delivered.

Command:

```text
./waf build --target=unit-tests -j2
./build/unit-tests --run_test=Spec175InvocationStreamLifecycle \
  --log_level=message --report_level=no
```

Result: `14/14` cases passed.

The one-Provider Normal integration fixture now attaches the consumer to the
real ServiceUser SVS/Interest window and verifies ordered callbacks, End, and
matching Response closure. This does **not** close T006: loss/retention,
retry, tamper, and callback-queue fault cases remain open and G1 remains
blocked.
