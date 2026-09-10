# Spec182 R10-B78 Provider Run-Limit Evidence

**Date:** 2026-09-09
**Branch:** `Experimental`
**Boundary:** standalone Provider source/static/build only; no protocol or qualification result

## Change and checks

`examples/DI_NativeProviderExecutable.cpp` now accepts `--run-for-ms` with `--serve`, emits
`NDNSF_DI_NATIVE_PROVIDER_RUN_LIMIT_REACHED`, shuts down the Face, and joins the provisioning thread.
`tests/python/test_spec182_native_closure.py` covers the parser and the absence of detached installation.

The focused Python closure test passed (`41 passed`). A fresh system-first Waf build of
`di-native-provider` passed 90/90 steps with `-j2`, exit `0`, elapsed `13:42.07`, maximum RSS
`2,860,420 KB`, and zero swaps; raw output is under `.codex-tmp/spec182-r10-b78-provider/`.

## Static boundary found

The serve loop observes the run limit, then calls `installThread.join()`. The installation thread
can still be polling Provider permission until `permissionWaitMs` (default 30 seconds) in
`examples/DI_NativeProviderExecutable.cpp:1864-1874`. Therefore the option currently supplies a
shutdown marker but does not guarantee process exit within `runForMs` when permission is absent.
No finite serve probe was classified in this batch; repair or cancel the permission wait before
using this option as a bounded independent-process harness.

## Status

`STATIC_PASS` and `BUILD_PASS` for the source/target boundary. `FOCUSED_BEHAVIOR_PASS` remains
pending. P1/P2/T010/T016 remain `PARTIAL`; no requester, Provider transport, or qualification claim
is made here.
