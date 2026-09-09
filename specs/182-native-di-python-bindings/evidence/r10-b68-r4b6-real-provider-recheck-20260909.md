# R10-B68 R4-B6 Real Provider Recheck

## Scope

本批次在 R10-B67 source checkpoint `606230fbe783217e140499bc7ac4e9a3e65f72b0` 上，
重建现有 integration target，并重新运行 R4-B6 的公开 native requester → Core →
real `ServiceProvider` fixture selectors。目标是确认 R10-B66/B67 的 contract and host
repairs 没有回退；这是本进程 fixture 验证，不是独立 Provider process、MiniNDN、
no-Python 或 T016 qualification。

## Validation

Build:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o build-nac182 build --targets=integration-tests -j2
  PASS; 118/118 tasks; target rebuilt from current source checkpoint
```

Runtime library identity used for the selectors:

```text
LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:\
/home/tianxing/NDN/nac-abe-integration-182/install/lib:\
/home/tianxing/NDN/ndn-svs/build
```

Selectors:

```text
Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation
  PASS; rc=0; SPEC182_NATIVE_DI_REQUEST_RESULT_OK

Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationReplacement
  PASS; rc=0; expected first-round NATIVE_REQUEST_STAGE_FAILED /
  DI_NATIVE_NO_ADMITTED_PROVIDER boundary observed and asserted

Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationAlternateReplacement
  PASS; rc=0; replacement conversation completed without test errors
```

Raw selector logs are retained under
`.codex-tmp/spec182-r10-b68-integration-20260909/`. `git diff --check` and
`validate_design.py --json` report no documentation errors. `vmstat 1 2` after the run
showed no swap-out in the second sample; the host still has allocated swap and the next
native build remains bounded at `-j2` unless a fresh measurement justifies otherwise.

The first local checkpoint commit attempt was stopped by the repository pre-commit hook's
generic full-index development-assistant text scan (existing `.specify/memory` references,
not this batch). The hook documents `NDNSF_LOCAL_CHECKPOINT=1` for local checkpoints; the
retry uses that flag and stages only this evidence plus the two Spec progress documents.

## Static and boundary review

The selector source and current R10-B66/B67 changes were re-read for Provider registration,
request identity ownership, replacement state, callback lifetime, and expected negative
boundaries. No new regression was found. The passing selectors close only the local R4-B6
real-Provider fixture boundary. They do not close T010/T011/T013/T014/T015/T016/T017,
maintained caller migration, independent Provider worker/cross-process transport, or
runtime Python exclusion.

## Closure decision

`CLOSED_FOR_VALIDATION` for this recheck; Spec182 remains `PARTIAL` and the next production
exit is an independently launched native requester → Core → Provider worker/cross-process
run with the mapped application/native request IDs observed at both boundaries.
