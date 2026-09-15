# B7R Lifecycle Fixes and Grant-Bound Cache Contract

**Date**: 2026-09-15 02:36 -05:00
**Base**: `acb3168d4ec10a4d13e12ff6ecaeeeacb9ba26bb`
**Scope**: T019 cache ownership, T020 conversation terminal admission, T021 generation identity and protected Provider cache contract.

## Static review

The immutable review snapshot was `.codex-tmp/spec185-lifecycle-fixes-review-v3`. Its manifest SHA256 is
`dbbe150f1c03f3fc5b509e01e19b43724ed5882719ea94f5c0e81d1edc201736`. The official
`/home/tianxing/.codex/skills/review-agent/SKILL.md` review returned `STATIC_PASS` with no P0-P3 findings.
The five lanes were all covered: ownership/lifetime, concurrency/state-order, compatibility/contract,
build/source closure, and C++ runtime-oracle. The review confirmed weak Runtime client indexing,
shared lazy PreparedModel client ownership, terminal rollback and `onTerminal` ordering, post-default
generation identity normalization, and grant-bound protected cache keys. It explicitly left production
protected Provider end-to-end cross-grant qualification unobserved.

## Implementation result

- Runtime stores weak client lookup/index entries; PreparedModel copies share a lazily materialized client
  owner. Pending handles remain strong owners, so eviction can release catalog/source bytes after views and
  handles are gone.
- Conversation clears its serial-turn gate from a process-local native terminal hook. `markTerminal`
  performs conversation abort and the hook before publishing the terminal event or completing Core, so a
  result waiter cannot race the next turn against delayed public completion notification.
- Conversation generation defaults are merged before `generationId`/`generation_id` are rebound to the
  current continuation. C-03 now states that these fields are per-turn identity and cannot be fixed model
  defaults.
- Protected Provider cache identity remains bound to `provider|grantName|grantDigest`. Independent grants
  therefore use separate entries; the contract does not claim cross-grant runner/template reuse. A future
  shared immutable source layer would still require per-request authorization and plaintext staging leases.

## Compile-link

Normal affected-target build used the existing `build-spec185-b0c-normal` tree, system-first
`PATH=/usr/bin:/bin:/usr/sbin:/sbin`, `CXX=/usr/bin/g++`, `WAFLOCK=.lock-spec185-b0c-normal`, and `-j4`:

```text
./waf build -j4 --targets=spec185-prepared-request,spec185-provider-assembly
```

It returned `rc=0` in 51.268 seconds. Raw log: `.codex-tmp/spec185-lifecycle-fixes-build-v1.log`;
return code: `.codex-tmp/spec185-lifecycle-fixes-build-v1.rc`. Binary SHA256 values are
`d02ac132466ecab08c0c4ae95c86e3b2b0b4c7d5e67e0bf57aa715ae223160d7`
(`spec185-prepared-request`) and
`f34f13bc789b9c66d438dd8f632d56194c1e6095c7ecff99bf33a9460fd7e07e`
(`spec185-provider-assembly`).

The independent ASan/UBSan build used `build-spec185-b3-asan-ubsan-fast`,
`WAFLOCK=.lock-spec185-b3-asan-ubsan-fast`, the same system-first toolchain and `-j4`; it returned
`rc=0` in 136.215 seconds. Raw log: `.codex-tmp/spec185-lifecycle-fixes-asan-build-v3.log`;
return code: `.codex-tmp/spec185-lifecycle-fixes-asan-build-v3.rc`. Binary SHA256 values are
`f28211307b028b5cc78d679d24bd5e9791ca00c70ae935ae4cf36c44ccdb23da`
(`spec185-prepared-request`) and
`9b9078181a13b30581b40cdd56920fc1031232a2b957c15f93a2100c093b8d49`
(`spec185-provider-assembly`).

## C++ runtime selectors

Normal and strict ASan/UBSan (`detect_leaks=1:halt_on_error=1:abort_on_error=1`) selectors all returned
`rc=0` and `*** No errors detected`:

| Selector | Normal log | Sanitizer log | Observed oracle |
| --- | --- | --- | --- |
| `Spec185ProviderAssembly/ProtectedArtifactCacheSeparatesIndependentGrantIdentities` | `.codex-tmp/spec185-lifecycle-fixes-grant-runtime-v1.log` | `.codex-tmp/spec185-lifecycle-fixes-grant-asan-runtime-v1.log` | same grant cold→hit, independent grant cold; `builds=2`, `templateHits=1` |
| `Spec185PreparedRequest/RuntimeClientDoesNotKeepEvictedSourceAlive` | `.codex-tmp/spec185-lifecycle-fixes-cache-runtime-v1.log` | `.codex-tmp/spec185-lifecycle-fixes-cache-asan-runtime-v1.log` | real C++ request/cancel/release, distinct preparation, source weak owner expires after `maxEntries=1` eviction |
| `Spec185PreparedRequest/PreparedConversationCommitsTwoNativeTurns` | `.codex-tmp/spec185-lifecycle-fixes-conversation-runtime-v1.log` | `.codex-tmp/spec185-lifecycle-fixes-conversation-asan-runtime-v1.log` | first result is followed immediately by a second turn; stale default generation identity is normalized |

Each `.rc` companion for the six runs contains `0`. The conversation selector uses the existing deferred
bridge fixture and proves the public two-turn sequence; an artificial callback-delay injection and a
separate failure-turn repetition remain unobserved. The direct grant selector is a cache-contract oracle,
not a claim that two independent protected production requests share assembled material; that end-to-end
measurement remains open for the later qualification batch.

## Composition review

The final B7R immutable snapshot `.codex-tmp/spec185-lifecycle-fixes-review-v4` received
`B7R_COMPOSITION_PASS` from the official review-agent, with no P0-P3 findings. Its manifest SHA256 is
`61527f2657fa760e9c1fd6c226883877fb69e191c2fa424739d3e271bfce4521`. The review checked the combined
code, C-03 contract, plan, task state, batch schedule and this evidence against the same affected C++
targets. It confirmed that `STOP_GROWTH / CLOSED_FOR_VALIDATION` closes only this validation batch;
T020/T021 and Spec185 remain partial for their explicitly listed unobserved lanes.

## Batch decision and miss retrospective

The lifecycle fixes form one stable B7R validation batch. Growth decision: `STOP_GROWTH / CLOSED_FOR_VALIDATION`.
The batch passed static review, compile-link, and focused C++ runtime-test lanes. Static review would have
missed compiler/source registration issues; compile-link would not establish eviction or terminal ordering;
runtime tests observe the C++ ownership and two-turn/cache-key oracles. Unobserved items are the protected
Provider cross-grant production request matrix, artificial delayed notification/failure-turn stress, and
the final T013 source/ELF/no-Python convergence. These limits keep T013 and the overall Spec185 status
`PARTIAL`.
