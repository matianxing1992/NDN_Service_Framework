# Spec185 B4 Conversation Evidence

**Status**: OPEN_FOR_NEXT_RETRY / `PARTIAL`
**Batch**: B4 (`T007 -> T008`)
**Base**: `775d0687d97eeb6c9f053d8e78c8c01b31864f91`
**Date**: 2026-09-13 (America/Chicago)

## Review trace

The official read-only skill was `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`).

- T007 static review: snapshot `.codex-tmp/spec185-t007-static-v7`,
  `STATIC_PASS`, DIFF SHA-256
  `01784b4058c3a562b1dd25a0dac76e7344cbfbe12fb1591a1c44d87b823c619f`.
- T008 v3 review: `STATIC_FAIL` for the shared fixture's secondary
  registration conflict.  The fixture was repaired so only `streaming=true`
  writes the conversation configuration.
- T008 v4 static review: snapshot `.codex-tmp/spec185-b4-t008-static-v4`,
  `STATIC_PASS`, DIFF SHA-256
  `1909a0e4ddc85153334ebe0e28d885976e43589899b91ba1d9e130d6a37b9249`,
  PATHS SHA-256 `a3f8d10cf00ee63bc853da5bf2ac83f3e09ede8e903ca64ae467a71e32cb82f3`.
- B4 composition review: snapshot `.codex-tmp/spec185-b4-composition-v1`,
  `B4_COMPOSITION_PASS / STATIC_PASS`, same base/diff/path identities as v4.
- Affected-range fixture repair review: snapshot
  `.codex-tmp/spec185-b4-after-fixture-static-v1`, `STATIC_PASS`, base
  `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
  `58b1b526c14864aa413a3c3c77f1a636b9d06d824d00e23c65755eaa69666fed`,
  PATHS file SHA-256
  `0d60fc539fb6761cf0883e7ed59767f5d7a1b48d78dfd927a4ed126883526d27`.
  The reviewer found no P0/P1/P2/P3 defect.  The five lanes confirmed the
  source-bound Qwen graph/state mapping, unchanged production API and
  coordinator, complete conversation target registration, and evidence
  boundary.  This review is static only; Qwen provider/ORT execution remains
  unobserved until the batch selector runs.
- Affected-range grant identity review: snapshot
  `.codex-tmp/spec185-b4-after-grant-static-v1`, `STATIC_PASS`, base
  `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
  `073e009b700966162727eee57a1821bb5e0fd28da631f1ac0c6a5060232e2f53`,
  PATHS file SHA-256
  `322f0dacea884f2363dd43e159eac91132e12f6e5041ceaf6f47e37857356069`.
  The reviewer found no P0/P1/P2/P3 defect and confirmed that the Qwen
  publication profile now matches `NativeGrantPublicationSource` while model,
  source, and manifest identities remain separate.  Dynamic qualification is
  still unobserved.
- Affected-range payload-bound review: snapshot
  `.codex-tmp/spec185-b4-after-payload-static-v1`, `STATIC_PASS`, base
  `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
  `cafaa202fa592c3d330876fce1182ab775bf9e158e458945b0512b8b427bdf6a`,
  PATHS file SHA-256
  `77775613c8a60db7bf106824eece37423e49a6a1163d2d65040ff0e511b48666`.
  The reviewer confirmed that only the Qwen branch uses the 4096-byte bound,
  the YOLO branch remains 32, and no production validation was weakened.
  Dynamic selector coverage remains unobserved.
- Affected-range payload-bound review: snapshot
  `.codex-tmp/spec185-b4-after-payload-static-v1`, `STATIC_PASS`, base
  `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
  `cafaa202fa592c3d330876fce1182ab775bf9e158e458945b0512b8b427bdf6a`,
  PATHS file SHA-256
  `77775613c8a60db7bf106824eece37423e49a6a1163d2d65040ff0e511b48666`.
  The reviewer confirmed that only the Qwen branch uses the 4096-byte bound,
  the YOLO branch remains 32, and no production validation was weakened.
  Dynamic selector coverage remains unobserved.

The five lanes were covered for the frozen composition: production callers;
coordinator/checkpoint wire and commit state; C++ fixture/oracle and target
registration; complete DI source/public-header closure; and migration,
lifecycle, and evidence boundaries.  Static review does not establish
compile-link or runtime qualification.  `BeforeDirectorySync` post-rename
failure injection remains an unobserved test boundary.

## First compile boundary

The first shared normal build used `/usr/bin/g++ -B/usr/bin`, `-j4`, and the
verified `build-spec185-b0c-normal` tree.  Command output and resource sample:

- `.codex-tmp/spec185-b4/normal-build-v1.log`
- `.codex-tmp/spec185-b4/normal-build-v1.meta` (`BUILD_RC=1`, 42 seconds)
- `.codex-tmp/spec185-b4/vmstat-normal-build.log`

Native conversation sources compiled.  The first failure was test translation
in `tests/integration-tests/di-prepared-request.t.cpp`: the fixture used an
unqualified `StreamFinishReason`, and `BOOST_CHECK_EQUAL` attempted to print
`std::vector<uint8_t>` checkpoint bytes.  This is recorded in
`docs/failure-log.md`; no runtime result is counted.  The repaired test uses
the qualified Core enum and boolean vector equality.  A fresh affected-range
static review and a new batch build are required.

The next shared build (`normal-build-v2.log`, `BUILD_RC=1`, 39 seconds) then
stopped at the first production translation of `Conversation.cpp`: `DiError`
was incomplete because only `PreparedModel.hpp` had been included.  The test
translation errors were gone.  This second failure is retained in
`docs/failure-log.md`; adding `Runtime.hpp` supplies the definition and still
requires an affected-range static review before retry.

## Runtime boundaries before fixture repair

The first selector invocation used an invalid comma-separated Boost.Test filter
and returned `200` without matching any case (`conversation-r1.log`).  The
corrected exact selector entered the real request path and returned `201` at
native planning (`conversation-r2.log`): `generation role omits a sealed state
input`.  This is a production contract failure, not a qualification result.
The conversation fixture combined a YOLO source graph with Qwen streaming
state names, so the native planner correctly rejected the role before
execution.

The repair uses the existing source-bound Qwen native-config ONNX/catalog for
the real conversation case and keeps the compact YOLO fixture for the separate
streaming cancellation/default probes.  Affected-range static review and a
fresh normal selector are required before any B4 PASS decision.

The first selector after that repair built successfully but returned `201` in
the authenticated request stage (`conversation-r3.log`):
`DI_PROTECTED_GRANT_REJECTED: published manifest differs from authorized
source`.  The Qwen catalog's profile digest did not match the grant fixture's
authorized publication source.  Production verification correctly rejected
the unbound publication.  The catalog now retains the Qwen source/model
identity while binding the explicitly authorized `fixture-profile`; the
affected range requires another static review before the next build.

The next exact selector reached native request preparation but returned `201`
(`conversation-r4.log`) with `native task payload exceeds the adapter byte
bound`.  The Qwen generation envelope is larger than the compact YOLO probe's
32-byte catalog limit.  The Qwen catalog now uses the maintained 4096-byte
bound while the ordinary YOLO fixture remains unchanged; another affected
static review and build are required.

The subsequent normal selector committed the first turn and passed checkpoint
export/import assertions, then returned `201` on the second turn at
`NativeConversationCoordinator::beginTurn` with `conversation append prefix
mismatch` (`conversation-r5.log`).  The public continuation reused the prior
canonical token list without adding the new input's token prefix, while the
coordinator correctly requires a strictly longer append prefix.  This exposes
an API/tokenizer binding gap; the coordinator validation remains intact and B4
is still `PARTIAL`.

## P1 static review boundary

The affected-prefix review for snapshot
`.codex-tmp/spec185-b4-after-prefix-static-v1` returned `STATIC_FAIL`: the
first repair exposed a P1 because `RequestOptions.canonicalTokenIds` was still
caller-writable, so a caller could submit a forged parent-extending vector.
The review covered all five lanes and performed no build/runtime work.  The
repair removes that public field.  `Conversation` now asks the verified native
adapter for the current input token suffix; the immutable catalog adapter owns
the encoder and adapters without a pinned encoder fail closed.  The Qwen
fixture declares its bounded operator-owned `OPAQUE_BYTE_TOKEN_IDS` input
contract, and the C++ conversation harness now derives all three prompt
prefixes from the actual `Input` bytes.  A new immutable affected-range static
review is required before retrying the batch.

That affected review found a second P1: the catalog constructor moved the
first entry's `std::function` into the adapter group and then inspected the
moved-from entry, so every pinned encoder was rejected as inconsistent before
preparation.  The fix snapshots the encoder-presence bit before the move and
compares that stable bit for each group member.  No build or runtime result is
counted until this range is reviewed again.

The corrected snapshot `.codex-tmp/spec185-b4-after-token-provenance-static-v3`
then passed the affected official review with no P0/P1/P2/P3 findings.  The
review verified the move-order repair, native adapter ownership, strict parent
prefix construction, Qwen fixture registration, complete DI source closure,
and lifecycle/evidence boundaries across all five lanes.  This remains static
only; compile/link and runtime selectors are still unobserved.

The B4 composition snapshot `.codex-tmp/spec185-b4-composition-v2` also
passed the official review with no findings.  It covered the complete
`Input -> verified adapter suffix -> parent continuation -> native client ->
coordinator begin/commit/checkpoint/recovery/export/close` flow, all five
lanes, and the model/source/grant/fixture identity and build registration
boundaries.  Compile/link and normal/sanitizer runtime remain unobserved until
the shared batch validation.

## Closure decision

`T007` and `T008` remain unchecked and `PARTIAL` until the shared normal and
sanitizer C++ selector runs prove two committed turns, recovery/replacement,
cancel/close, export failure preservation, and the documented lifecycle
matrix.  No Python, Provider, process qualification, SIF, Tiger, or MiniNDN
result is implied by this record.

## Latest runtime boundary after outer-pump repair

The direct expected-exception repair for the second request was reviewed from
immutable snapshot `.codex-tmp/spec185-b4-after-concurrent-exception-static-v1`
with `STATIC_PASS` and no P0/P1/P2/P3 findings.  The snapshot uses base
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`510d4b549c4d95930e1243880d417c3b3dcd1b89bbe341a75c0c1ea224a0f28e`, and
PATHS SHA-256
`ca0eddda778839d65cef1bfd1b5e6f8e534a74fab8edecbcfe49d680445f1e82`.
The review covered the five lanes and confirmed that only the C++ fixture
exception harness changed; it did not weaken the production
`CONVERSATION_TURN_IN_PROGRESS` contract.

Normal build v15 and v16 of `spec185-prepared-conversation` both completed with
`BUILD_RC=0`, `/usr/bin/g++ -B/usr/bin`, and `-j4`.  Conversation selectors
`conversation-r31.log` and `conversation-r32.log` each passed all 20
assertions; API selectors `api-r33.log` and `api-r34.log` each passed their
single assertion.  These runs cover the native committed-turn,
checkpoint/export, recovery/replacement, close/drain, and unauthenticated
checkpoint cases on the normal target.

Sanitizer builds v10 and v11 also completed with `BUILD_RC=0` and
`SANITIZERS=address,undefined`.  The API selector passed in r13/r14, r15/r16,
and r17/r18.  The conversation selector remains unresolved: r11 returned
`RC=134` with `*** stack smashing detected ***` and the ASan warning about
`__asan_handle_no_return`; r12 returned `RC=1` with a nested null-read
`AddressSanitizer: DEADLYSIGNAL`; r15 returned the same stack-smash boundary;
r16 passed all 20 assertions; r17 and r19/r20 again returned the stack-smash
boundary, while r18 returned the nested null-read boundary.  The failures
occur at the expected second-request exception point (the test has only five
of six early assertions before abort), and no production requester or
coordinator frame, UBSan report, or independent protocol error appears in the
raw logs.  A diagnostic no-abort run passed, but its relaxed sanitizer policy
cannot qualify the batch.  These results establish intermittent sanitizer
failure in the C++ fixture/exception path, so T007/T008 remain `PARTIAL` and
the normal PASS is not promoted to B4 runtime PASS.

The raw selector logs, build metadata, and the earlier GDB records remain under
`.codex-tmp/spec185-b4/asan-runs/` and `.codex-tmp/spec185-b4/`.  The next
action is to isolate the sanitizer exception/unwinding boundary without
changing production close or callback semantics, then repeat affected static
review, one shared rebuild, and strict ASan/UBSan selectors.

The isolated exception-boundary repair was reviewed from immutable snapshot
`.codex-tmp/spec185-b4-after-thread-exception-static-v2` with `STATIC_PASS` and
no P0/P1/P2/P3 findings.  The snapshot uses base
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`76c95903e9a125749339267ee51b587c039e3bcb39e8ddf171598113cfd69db5`, and
PATHS SHA-256
`659ecd93d4856e8610c7f2c318522a699869a54721b44ab4ed7b6dc3950a2788`.
The reviewer confirmed that the competing request is caught inside its joined
C++ thread, while the production active-turn mutex, completion callback,
close/cancel race, and `PreparedModel -> NativeInferenceClient` ownership are
unchanged.  This remains static only; compile-link and runtime-test lanes are
unobserved until the shared rebuild and strict selectors complete.

## Latest sanitizer boundary after thread-exception repair

The first strict ASan/UBSan selector after the thread-exception repair was
`.codex-tmp/spec185-b4/asan-runs/conversation-r21.log` (`RC=134`, about 5.2s).
It still terminated with `*** stack smashing detected ***` at the expected
second-request assertion (`di-prepared-request.t.cpp:1303`), followed by the
ASan `__asan_handle_no_return` warning and leak output caused by abort.  A
repeat with `detect_stack_use_after_return=0` (`conversation-r22-no-stack-uarr.log`)
returned the same `RC=134` stack-smash boundary, so the fake-stack option is
not its cause.  The normal target and API selector remain passing, but these
strict conversation runs do not qualify B4.

The diagnostic GDB run
`.codex-tmp/spec185-b4/asan-runs/asan-gdb-thread-exception-stack3.log`
breaks `__stack_chk_fail` on the main thread in
`NdnsfIntegrationEnvironment::pumpUntil`, with no production requester or
conversation-coordinator frame.  This narrows the unresolved boundary to the
C++ fixture pump/exception interaction; it does not establish a production
memory defect or a protocol result.  T007/T008 therefore remain `PARTIAL`.
The next repair will remove promise/future bookkeeping from the competing
request probe while preserving the same C++ thread and
`CONVERSATION_TURN_IN_PROGRESS` assertion, then repeat static review, one
shared rebuild, and strict selectors.

The promise/future removal was reviewed from immutable snapshot
`.codex-tmp/spec185-b4-after-atomic-exception-static-v1` with
`STATIC_PASS` and no P0/P1/P2/P3 findings.  The snapshot uses base
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`f0782804e73bae6c5cf2e5f8415908633f4fa83901ce2adea99a930e206ef7d5`, and
PATHS SHA-256
`cf6a3445cfd71d355aa8e593dcda40b9ba72abaaf8f245ebd60a12e4143357b3`.
The official `review-agent` confirmed that the joined C++ thread keeps
`conversation` and `options` alive, catches all exceptions, and reads the
atomic result with acquire ordering after join; production active-turn,
completion, close/cancel, and model/client ownership are unchanged.  The
compile-link and runtime-test lanes remain unobserved until the shared rebuild
and strict selectors complete.

After the static pass, normal build v19 completed with the verified original
Waf lock and `-j4`; conversation r37/r38 and API r37/r38 each passed.  The
fresh ASan/UBSan build v13 also completed with `BUILD_RC=0`, but conversation
r23 still returned `RC=134` after about 5.6s with `*** stack smashing
detected ***` at the atomic concurrent-request assertion (line 1306), the
same `__asan_handle_no_return` warning, and abort-triggered leak output.  This
shows that removing promise/future bookkeeping did not resolve the strict
sanitizer boundary; no production requester/coordinator diagnostic was
reported.  The raw run is `.codex-tmp/spec185-b4/asan-runs/conversation-r23.log`
with metadata beside it.  T007/T008 remain `PARTIAL` pending another fixture
probe reduction and strict repeat.

Strict ASan/UBSan conversation r24 passed all 20 assertions with `RC=0` and
`*** No errors detected`, but immediate repeat r25 returned `RC=134` with the
same stack-smash boundary at the atomic concurrent-request assertion.  The
pair therefore demonstrates intermittent behavior rather than two consecutive
qualification runs.  Raw logs and metadata are
`.codex-tmp/spec185-b4/asan-runs/conversation-r24.log` and
`conversation-r25.log`; T007/T008 remain `PARTIAL`.

The controlled direct-exception isolation was reviewed from immutable snapshot
`.codex-tmp/spec185-b4-after-direct-exception-static-v1` with `STATIC_PASS` and
no P0/P1/P2/P3 findings.  The snapshot uses base
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`33292d8b7b1c1be8d2d49deddff6dfeb1a386523d9370553ee1451059a3bbb81`, and
PATHS SHA-256
`cf6a3445cfd71d355aa8e593dcda40b9ba72abaaf8f245ebd60a12e4143357b3`.
The reviewer confirmed the active-turn rejection, production mutex and
completion/close lifetime remain correct.  The direct probe intentionally
leaves real cross-thread competition unobserved; this is an evidence limit,
not a static defect.  Compile-link and strict runtime lanes remain unobserved
until the new shared build and selectors complete.

The first compile-link attempt after this review stopped before compilation:
normal build v18 returned `BUILD_RC=1` because Waf reported that
`build-spec185-b0c-normal` was not configured.  The raw command output is
`.codex-tmp/spec185-b4/normal-build-v18.log` with metadata in
`.codex-tmp/spec185-b4/normal-build-v18.meta`; no source or runtime result is
inferred.  The next step is a fresh configuration using the verified system
toolchain, followed by the single affected-target build.

The direct-exception isolation then reached a fresh compile-link and normal
runtime boundary.  Normal build v20 used the existing `.lock-spec185-b0c-normal`
tree with `-j4`, returned `BUILD_RC=0`, and took `25.611s`; metadata and output
are `.codex-tmp/spec185-b4/normal-build-v20.meta` and
`.codex-tmp/spec185-b4/normal-build-v20.log`.  The native conversation selector
`Spec185PreparedRequest/PreparedConversationCommitsTwoNativeTurns` passed all
20 assertions in consecutive runs r39 and r40 (`RC=0`, `*** No errors
detected`); raw logs and metadata are under
`.codex-tmp/spec185-b4/normal-runs/conversation-r39.*` and `conversation-r40.*`.

The matching ASan/UBSan + LSan target v14 also compiled and linked with
`BUILD_RC=0` in `40.614s` using `.lock-spec185-b3-asan-ubsan-fast` and `-j4`.
Strict direct-isolation run r26 returned `RC=134` with
`*** stack smashing detected ***` at the expected active-turn assertion, and
immediate run r27 returned `RC=1` with nested `AddressSanitizer: DEADLYSIGNAL`;
neither log contains a requester/coordinator diagnostic.  The independent
sanitized API selector
`Spec185PreparedConversationApi/ConversationCheckpointRejectsUnauthenticatedBytes`
passed consecutively in r19 and r20.  These results confirm the normal
conversation path and API boundary, but the strict sanitizer conversation lane
is still intermittent and cannot qualify B4; T007/T008 remain `PARTIAL`.

## Batch build (compile-link)

After the B4 composition static pass, the shared affected target was rebuilt
once with the verified system toolchain and `-j4`:

- target: `build-spec185-b0c-normal/spec185-prepared-conversation`
- command boundary: `./waf build --targets=spec185-prepared-conversation -j4 -v`
- result: `BUILD_RC=0`, Waf elapsed `1m26.556s`
- records: `.codex-tmp/spec185-b4/normal-build-v7.log`,
  `.codex-tmp/spec185-b4/normal-build-v7.meta`,
  `.codex-tmp/spec185-b4/vmstat-normal-build-v7.log`

All 108 sources in the affected DI target compiled and linked.  The compiler
reported only existing warnings; no build error or sustained swap activity was
observed.  Runtime selectors and sanitizer validation remain unobserved.

## Runtime boundary after compile-link

The first post-build normal conversation selector (`conversation-r6.log`,
`RC=201`, about 41 seconds) entered the real request path and reached the
checkpoint assertion, but failed at `last checkpoint` with
`final payload disagrees with accepted generation`.  This is a native
accepted-generation/final-payload binding failure, so it is not a
qualification result.  The batch remains `PARTIAL`; the exact log and meta
record are retained under `.codex-tmp/spec185-b4/normal-runs/`.

The fixture repair changes only `tests/integration-tests/di-prepared-request.t.cpp:1140`:
the append final `tokenIds` now reuse the emitted `nextToken`.  The immutable
affected snapshot `.codex-tmp/spec185-b4-after-fixture-token-static-v1` was
reviewed by the official read-only agent with `STATIC_PASS` and no P0/P1/P2/P3
findings (base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`b7060fa95c367d943d86b2bfd3a8d988509db3e07a0ff653a3a4e8e6bc5843e8`, PATHS
SHA-256 `46b6907ad4138ed7c600124f5cefa0f46bd364f5c937c64ee9f0e84d3f999cd2`).
The review covered all five lanes; compile-link and runtime remain pending.

The first repaired normal selector (`conversation-r9.log`) passed with
`RC=0`, including three committed turns, checkpoint export/import, recovery,
replacement, and failed-export preservation.  The required repeated run
(`conversation-r10.log`) reached the restored third turn but its 8-second
`RequestHandle::result` observation window expired with
`native result wait timed out` (`RC=201`).  No new protocol or validation
error was reported.  This is retained as a runtime timing boundary; the
fixture's request/observation budget is being widened within a bounded test
contract before another retry.

The first ASan/UBSan selector attempts (`asan-runs/conversation-r1.log` and
`conversation-r2.log`) also returned `RC=201` with the same
`native result wait timed out` boundary while collecting the first committed
turn.  The sanitizer binary was built successfully (`asan-ubsan-build-v5`,
`BUILD_RC=0`), and neither log contains an ASan, UBSan, or LSan diagnostic.
The failure is therefore retained as an observation-budget boundary rather
than a sanitizer finding; the test now gives the unoptimised sanitizer build
a separately bounded 60-second request/45-second result budget.  These
attempts do not establish runtime PASS.

The budget repair was reviewed again from immutable snapshot
`.codex-tmp/spec185-b4-after-sanitizer-budget-static-v4`, `STATIC_PASS`, with
no P0/P1/P2/P3 findings.  Its base is
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`ffb13b99ba4d46562df7411d4fd0a63d2ba6991e4479bae91e03f19540df8ffc`, and
PATHS file SHA-256
`8a89316bfb35be850175cdd846c8c7377d121eb304c02245f55571481ec439a0`.
The review covered all five lanes and confirmed that both GCC and Clang ASan
select the sanitizer budget; this remains a static result only.

After that review, the affected normal target was rebuilt once more with
`/usr/bin/g++ -B/usr/bin`, `-j4`, and the same `build-spec185-b0c-normal`
tree.  `normal-build-v9.meta` reports `BUILD_RC=0`; the detailed log and
resource sample are `.codex-tmp/spec185-b4/normal-build-v9.log` and
`.codex-tmp/spec185-b4/vmstat-normal-build-v9.log`.  The exact conversation
selector passed twice (`conversation-r11.meta` and `conversation-r12.meta`,
both `RC=0`, `No errors detected`), including three committed turns,
checkpoint export/import, recovery, replacement, and failed-export
preservation.  The API selector for unauthenticated checkpoint bytes also
passed twice (`api-r13.meta` and `api-r14.meta`, both `RC=0`).

The post-review normal repeat (`normal-runs/conversation-r16.log`) returned
`RC=201` at the first-turn result observation with
`native result wait timed out` after five early assertions; the preceding
repeat (`conversation-r15.log`) passed.  This is the same host scheduling
boundary seen in r10 and the sanitizer attempts, with no new protocol error or
sanitizer diagnostic.  The fixture budget is therefore widened uniformly to a
bounded 60-second native request and 45-second result observation for both
normal and sanitizer builds, followed by another static review and rebuild.

The uniform-budget repair was reviewed from immutable snapshot
`.codex-tmp/spec185-b4-after-uniform-budget-static-v1`, `STATIC_PASS`, with no
P0/P1/P2/P3 findings.  Its base is
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256
`7ad642eb0ae2b1cdfb7f65388bb9dd9d85e3a86bbe16fd1724e945dc2f8878fc`, and
PATHS file SHA-256
`a9aa9e2c4b32d58ba5f797d287e9c360051615361c561651cd05a86da89821fa`.
The five lanes confirmed that the larger observation budget preserves the
native request deadline and all conversation state invariants; no compile or
runtime result is inferred from this static pass.

The first normal run after the uniform-budget review (`normal-runs/conversation-r19.log`)
still reached the 45-second result-observation boundary and returned `RC=201`
with `native result wait timed out`; the immediate repeat (`conversation-r20.log`)
passed.  No protocol, ownership, or sanitizer diagnostic was reported.  The
fixture therefore retains both raw runs and raises its explicit bounded budget
to a 180-second native request and 120-second result observation before the
next static review and build.

After the extended-budget static pass, normal build v12 and sanitizer build v7
both succeeded.  Normal conversation r23/r24 and API r25/r26 each passed
twice.  In contrast, sanitizer conversation r7 and r8 both returned `RC=201`
at the first-turn result observation (`122s` and `181s` respectively) with
`native result wait timed out`; sanitizer API r7/r8 both passed.  The raw
conversation logs contain no ASan, UBSan, or LSan report.  These runs establish
an unresolved sanitizer liveness/observation boundary, so T007/T008 remain
`PARTIAL`; no runtime qualification PASS is inferred from the normal selectors.

Source inspection of the failed sanitizer path identified the first stalled
boundary in the C++ fixture: `NdnsfIntegrationEnvironment::pumpUntil()` stops
after 200 five-millisecond face rounds (about four seconds), then the test
blocks in `future.get()` while the DummyFace event queues are no longer pumped.
The sanitizer request can exceed that initial pump window, so its result wait
expires without a sanitizer diagnostic.  The repair adds a bounded C++ helper
that repeats those pump chunks until the result future is ready or the native
request deadline is reached; production request/coordinator code is unchanged.

The first compile after this repair (`normal-build-v13.meta`) stopped in test
translation (`BUILD_RC=1`): the new helper used the unqualified
`NdnsfIntegrationEnvironment` name, while the fixture declares it under
`ndn_service_framework::test`.  No product source or runtime result is
implicated; the helper's type is being qualified before another static review.

The corrected helper was reviewed from immutable snapshot
`.codex-tmp/spec185-b4-after-pump-helper-static-v3` with `STATIC_PASS` and no
P0/P1/P2/P3 findings.  The snapshot base is
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256 is
`d5c853292f80cf3792cff3afce9dd146306d14938780862aa238b0a158f1b083`, and
the PATHS.sha256 file SHA-256 is
`06208704a8a0735b485f6fe76462e1abc8be1c356c59da84995fb83c69e9d6ad`.
The review covered the five lanes, future/fixture lifetime, bounded request and
result budgets, and confirmed that only the C++ test helper changed; compile
and runtime lanes remained unobserved at this point.

After that review, normal build v14 of `spec185-prepared-conversation` with
`/usr/bin/g++ -B/usr/bin` and `-j4` succeeded (`BUILD_RC=0`,
`.codex-tmp/spec185-b4/normal-build-v14.log`).  Conversation selectors r27 and
r28 and API selectors r29 and r30 each returned `RC=0` with no Boost.Test
errors.  These normal runs covered the three committed turns, checkpoint
export/import, recovery/replacement, and unauthenticated checkpoint rejection.

Sanitizer build v9 also succeeded (`BUILD_RC=0`,
`.codex-tmp/spec185-b4/asan-ubsan-build-v9.log`).  The sanitizer API selectors
r9 and r10 passed, but conversation selectors r9 and r10 both returned
`RC=1` after about five seconds with an AddressSanitizer `DEADLYSIGNAL` write
at `NdnsfIntegrationEnvironment::pumpUntil` (`asan-conversation-r9.log`,
`asan-conversation-r10.log`).  There was no production request/coordinator
frame and no UBSan or LSan diagnostic.  A bounded gdb run with
`handle_segv=0` (`.codex-tmp/spec185-b4/asan-gdb-pump.log`) places the main
thread at `pumpUntil` while worker threads are idle in their queues; a second
register/disassembly run (`asan-gdb-pump-regs.log`) identifies the failing
instruction as the sanitizer stack-frame cleanup write.  This is a real
sanitizer failure in the test fixture path, not a timeout or qualification
PASS.  T007/T008 therefore remain `PARTIAL` while the outer-pump repair is
validated with a new static snapshot and fresh builds.

The next repair adds an opt-in `BootstrapProfile::deferBridgeDelivery` mode to
the C++ integration fixture and enables it only for the Spec185 conversation
profile.  Deferred packets are copied into the destination Face's
`io_context`; the default inline bridge remains unchanged.  Repair-only
snapshot `.codex-tmp/spec185-b4-after-deferred-bridge-static-v6` passed the
official read-only `review-agent` with `STATIC_PASS` and no P0/P1/P2/P3
findings.  The snapshot base is
`775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256 is
`b901a0c8cf1274b4cd12993dccb954094e114e54851ec603924eb56eb5366003`, and
PATHS SHA-256 is `254697261dd357f8d631d85598932e5c18c26b695a365399a192945f179a46a0`.
The review confirmed packet ownership, FIFO/fault ordering, destination-context
pump reachability, sanitizer re-entry isolation, and default compatibility;
queued-handler exception and deferred fault/reorder combinations remain
runtime lanes.

For diagnosis only, a repair-reviewed environment switch temporarily skipped
the active-turn exception probe.  After the corresponding normal v22 and
ASan/UBSan v16 rebuilds, strict run
`SPEC185_SKIP_CONVERSATION_BUSY_PROBE=1` completed the full three-turn,
checkpoint, recovery, export, close, and drain path with `RC=0` and no
sanitizer errors (`.codex-tmp/spec185-b4/asan-runs/conversation-r32-skip-busy.*`).
The switch was then removed, restoring the original mandatory
`CONVERSATION_TURN_IN_PROGRESS` assertion.  This isolates the unresolved
boundary to that real C++ exception path under the current sanitizer/dependency
combination; the diagnostic run is not qualification evidence.

After this static gate, normal build v21 on `.lock-spec185-b0c-normal` with
`-j4` succeeded in `31.529s`; conversation r41 and r42 each passed all 20
assertions (`RC=0`).  ASan/UBSan + LSan build v15 on
`.lock-spec185-b3-asan-ubsan-fast` also succeeded in `45.586s`, but strict
conversation r31 again returned `RC=134` with stack-smash at the active-turn
assertion despite deferred bridge delivery.  The repeated failure still has no
production requester/coordinator frame; the sanitizer conversation lane is
therefore unqualified.  The next step is a diagnostic build that skips only
the active-turn assertion to determine whether the failure is tied to that
exception path; it is not a qualification run and T007/T008 remain `PARTIAL`.

## Final B4 composition and qualification

The final B4 immutable composition snapshot is `.codex-tmp/spec185-b4-composition-v3`.
Its base is `775d0687d97eeb6c9f053d8e78c8c01b31864f91`, DIFF SHA-256 is
`ff51c4f7750cf97072a30cba224db25093b3a14c55146ba89d2948edbd32dbaa`, and
PATHS SHA-256 is
`f84fb44ec6bd5ef0a8eba86247eb1b7a744e0986eafc7d7f19044cf587fd2568`.
The official read-only `review-agent` reported
`B4_COMPOSITION_PASS / STATIC_PASS` with no P0/P1/P2/P3 findings.  The review
covered production entry/callers, wire/state behavior, C++ fixture/oracle,
build/source closure, migration/evidence, and the required ownership,
concurrency, error, compatibility, and security checks.  Broader deferred
fault/reorder pressure and true scheduler-level cross-thread interleavings are
unobserved and are not claimed as covered by this batch.

After the composition gate, the shared normal build v26 on
`.lock-spec185-b0c-normal` (`-j4`) completed with `BUILD_RC=0`; the ASan/UBSan
+ LSan build v19 on `.lock-spec185-b3-asan-ubsan-fast` also completed with
`BUILD_RC=0`.  The build logs and vmstat captures are
`.codex-tmp/spec185-b4/normal-build-v26.log`,
`.codex-tmp/spec185-b4/vmstat-normal-build-v26.log`,
`.codex-tmp/spec185-b4/asan-ubsan-build-v19.log`, and
`.codex-tmp/spec185-b4/vmstat-asan-ubsan-build-v19.log`.

The post-composition C++ selector runs then passed twice in each required
configuration.  Normal conversation r49/r50 and API r51/r52 used
`build-spec185-b0c-normal`; strict ASan/UBSan + LSan conversation r40/r41 and
API r42/r43 used `build-spec185-b3-asan-ubsan-fast`.  Every run returned
`RC=0`, reported `*** No errors detected`, and emitted the expected
`NDNSF_INTEGRATION_BOOTSTRAP_READY` receipt where applicable.  Logs are under
`.codex-tmp/spec185-b4/normal-runs/` and `.codex-tmp/spec185-b4/asan-runs/`.

The native C++ path now has observable coverage for two committed conversation
turns, checkpoint export/import, recovery and replacement isolation, close and
drain, and unauthenticated checkpoint rejection.  The readiness-predicate and
deferred-bridge edits are test-fixture changes; production conversation,
coordinator, journal, and request ownership remain unchanged.  Python binding
acceptance is outside B4 and remains a later T012/B8 obligation.

T007 and T008 are therefore closed for this batch with `PASS` at the stated
scope.  The unobserved lanes above remain explicit limits and do not promote
the overall Spec185 status beyond `PLANNED`.
