# Spec182 R10-B78 Provider Run-Limit Evidence

**Date:** 2026-09-09
**Branch:** `Experimental`
**Boundary:** standalone Provider source/static/build only; no protocol or qualification result

## Change and checks

`examples/DI_NativeProviderExecutable.cpp` now accepts `--run-for-ms` with `--serve`, emits
`NDNSF_DI_NATIVE_PROVIDER_RUN_LIMIT_REACHED`, shuts down the Face, and joins the provisioning thread.
`tests/python/test_spec182_native_closure.py` covers the parser and the absence of detached installation.

The focused Python closure test passed (`41 passed`). The current requester/Provider target
invocation passed a system-first Waf build with `-j2`, exit `0`; Waf entered the existing
`.codex-tmp/spec182-r4-b2/build` tree, rebuilt the requester, and recorded maximum RSS
`1,133,028 KB` with zero swaps. Raw output is `.codex-tmp/spec182-r10-b80-build.log`; the
current Provider artifact from the same-source target build is
`1e717bca3eab12f7bcdf5b35a237ec98fda2769c9047b45bf4fa1a374ee3c3a0`.

## Static boundary and repair

The original serve loop observed the run limit, then called `installThread.join()` while the
installation thread could still poll Provider permission until `permissionWaitMs`. The repair
adds a shared release/acquire run-limit fence; the permission loop emits
`NDNSF_DI_NATIVE_PROVIDER_PERMISSION_WAIT_CANCELLED reason=run-limit`, signals completion, and
returns before the main thread joins it.

The requester `--help` text was also corrected in the same audit follow-up to state the actual
current-working-directory versus config-relative path rules. Its output is retained in
`.codex-tmp/spec182-r10-b80-requester-help.log` with exit `0`.

## Finite serve probe

Using the four-role `spec174-exact-bundle-gpu-v5` plan, the R10-B55 metadata-only manifest, the
fresh Provider binary, and a real local `App_ServiceController`, the Provider was launched with
`--run-for-ms 600` and `--permission-wait-ms 4000`. The controller supplied public parameters
and the Provider reached `SERVE_READY`; at the limit it emitted `RUN_LIMIT_REACHED` and the
installer emitted `PERMISSION_WAIT_CANCELLED`. Provider and Controller both exited `0` in
`5.293s` wall time (the controller was intentionally kept alive for 5 seconds). Raw output is
retained under `.codex-tmp/spec182-r10-b80-provider-run-limit-controller/`.

## Status

`STATIC_PASS`, `BUILD_PASS`, and `FOCUSED_BEHAVIOR_PASS` for the bounded standalone Provider
lifetime and requester CLI help boundary. This does not claim requester/Provider transport,
worker/cross-process behavior, continuation, no-Python migration, or T016 qualification; P1/P2/
T010/T016 remain `PARTIAL`.
