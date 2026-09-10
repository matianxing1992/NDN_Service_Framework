# R11-B8-G6 Native Process Fixture Startup Boundary

**Date**: 2026-09-10
**Status**: `BLOCKED` at fixture startup; no protocol result
**Parent**: R11-B8 / R11-B4 / T016

## Scope

Attempted the maintained C++ stream conversation process with the freshly
linked `App_ServiceController`, `DI_NativeArtifactAuthority`,
`di-native-provider`, `DI_NativeRequester`, and `DI_NativeOnnxAssemblyWorker`.
The command requested two native turns, Provider restart recovery, and a wrong
parent-checkpoint negative.

## First Boundary

The driver retained its run root at
`.codex-tmp/spec182-r16-native-process-20260910/stream-conversation-recovery/`.
NFD was invoked with the generated configuration, but exited before creating
the Unix socket with `File name too long`. The generated socket pathname
exceeded the Unix-domain pathname limit because the retained run root was too
deep. The driver was stopped while waiting for that socket (exit 130).

No Controller, Authority, requester, Provider, grant, selection, stream, or
conversation protocol result is counted from this attempt.

## Corrective Gate

Retry with the same binaries and configuration under a short `/tmp` run root,
then retain/copy the raw logs under the Spec182 `.codex-tmp` run record. Do not
change production code for this fixture-path failure.

## Raw Evidence

- `.codex-tmp/spec182-r16-native-process-20260910/stream-conversation-recovery/nfd.log`
- `.codex-tmp/spec182-r16-native-process-20260910/stream-conversation-recovery-driver.log`
