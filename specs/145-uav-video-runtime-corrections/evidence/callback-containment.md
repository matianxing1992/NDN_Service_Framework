# T003 GStreamer Callback Containment

## Red

The injected callback tests initially failed to compile because the pipeline
did not expose a retained failure record. The old C callbacks also invoked APP
callbacks directly and allowed C++ exceptions to cross the GStreamer C ABI.

## Implementation

Capture, decode, and raw-frame callback entry points now use a common
fail-closed adapter. It:

- catches both `std::exception` and non-standard exceptions;
- retains only the first bounded failure record;
- changes `Running` to `Failed`;
- suppresses later APP callback invocation;
- returns `GST_FLOW_ERROR` (or drops the raw-frame probe);
- leaves teardown to the owning loop, outside the callback stack;
- permits repeated `stop()` safely.

## Green

Commands:

```bash
./waf build --targets=unit-tests,UavDroneApp -j2
./build/unit-tests \
  --run_test=UavProtocolState/UavGStreamerCaptureCallbackExceptionsFailClosed
./build/unit-tests \
  --run_test=UavProtocolState/UavGStreamerDecodeCallbackExceptionsFailClosed
```

Result: PASS for standard and non-standard capture/decode exceptions,
single first-failure notification, later-callback suppression, failed-state
retention, decode refusal after failure, and idempotent stop.
