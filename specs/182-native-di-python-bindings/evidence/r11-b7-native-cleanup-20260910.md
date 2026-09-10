# R11-B7 Native Cleanup Process

**Date**: 2026-09-10
**Status**: CLOSED_FOR_VALIDATION (bounded native lifecycle and cleanup出口; parent qualification remains open)
**Scope**: terminal, cancellation, deadline, replacement drain, secret ownership and shared Provider host isolation

## C++ lifecycle evidence

The current C++ unit binary passed these selectors from the R11-B6 source build:

```text
Spec182NativeInferenceClient/*   2 cases
Spec182NativeRequestIdentity/*   1 case
Spec182Registration/*            6 cases
Spec182SharedLease/*              3 cases
Spec182ProviderHost/*             7 cases
Spec182StreamAcceptance/*         7 cases
```

Every selector exited `0` with `*** No errors detected`. The client cases cover operation
compaction, cancellation during preparation, slow-observer cancellation, absolute deadline,
timer removal, late expiry, client close and retained-handle ownership. Registration and host
cases cover close-before-ACK, old selection after re-register, exact pending cleanup, provider
destruction, successor preservation, shared lease conflicts, target binding, safe release,
close/re-serve generation fencing, stop idempotence and late ACK rejection. The stream selector
covers terminal fencing, rejected/duplicate events, callback failure, commit-failure prefix
recomputation and exclusion of unreceived tokens from replacement.

Native secret cleanup remains in the C++ owners: conversation authentication keys are cleansed
by `NativeConversationCoordinator`, protected content/grant material by
`NativeProviderRuntime`/`NativeGrantVerifier`/`NativeProtectedArtifactStore`, and provider
registration/lease state is released only at its safe fence. No Python test or callback owns
these bytes.

## Cross-process drain evidence

The R11-B6 normal, replacement-success, no-backup and conversation/recovery processes all ran
their `finally` cleanup. A post-run process scan found no `DI_NativeRequester`,
`di-native-provider`, `DI_NativeArtifactAuthority` or `App_ServiceController` process. The
replacement-success run showed Provider A stopped before execution and Provider B completed
`attempt-2`; the no-backup run terminated once at
`NATIVE_REQUEST_STAGE_FAILED`/`DI_NATIVE_NO_ADMITTED_PROVIDER`. The C++ integration selector
`Spec170NdnsfDiCoreFlow/Spec182*` passed 9/9 after these changes.

These checks establish the bounded native cleanup and shared-host出口. They do not claim every
T016 isolation/PO case, maintained caller migration, no-Python qualification, or whole-Spec
completion.
