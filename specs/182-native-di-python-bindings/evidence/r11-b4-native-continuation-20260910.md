# R11-B4 Native Continuation Process — Failure Boundaries

**Date**: 2026-09-10
**Status**: CLOSED_FOR_VALIDATION (bounded independent C++ process; parent qualification remains open)
**Scope**: C++ `DI_NativeRequester` continuation harness and independent C++ Provider process

## First boundary

The first `--conversation` run reached the real C++ Provider execution path and emitted
grant verification plus ONNX Runtime CPU execution evidence, then failed at
`NativeProviderRuntime::stageDecodeStatePromotion` with
`PROVIDER_CONVERSATION_PROMOTION_STAGE_FAILED`. The requester consequently reported a
stream gap timeout. The journal remained empty because the first turn never reached a
committed conversation checkpoint.

Raw run: `/tmp/spec182-r11-b3-probe-9fwp2_mg/` (`requester.log`, `provider.log`).

## Diagnosis

The first streamed turn defers decode-state commit while the conversation role set is
awaiting requester `COMMIT`. The CPU ONNX promotion branch only queried the committed
decode-state map, so it could not see the request-local candidate. In addition, the
candidate-only entry could not be removed by the existing session/role erase path after
conversation commit. This is a C++ state-ownership defect; Python is only the process
lifecycle driver.

## Repair in progress

The C++ store now has an exact `lookupCandidate` path for deferred promotion and its
erase operation accounts for candidate-only entries. The standalone driver also avoids
masking a first-process failure with a missing second log. Rebuild and the two-round
FULL_CONTEXT → APPEND_DELTA retry remain required before this batch can be closed.

## Second boundary

After the C++ promotion repair, a fresh process retry published and authenticated the
Provider receipt, then the requester rejected the assembled transcript at
`conversation receipt lineage mismatch`. The Provider's decode identity correctly
included the input prefix token `[3]`, while the standalone continuation fixture declared
an empty canonical prefix. This is a fixture-to-C++ lineage contract mismatch; it is not
Python business logic. Raw output is retained under
`/tmp/spec182-r11-b3-probe-vyoahjfs/` (`requester.log`, `provider.log`).

The fixture now declares `[3]` as the FULL_CONTEXT canonical prefix. The next fresh run must
prove receipt/lineage validation, requester `COMMIT`/`FINALIZE`, a persisted checkpoint,
an APPEND_DELTA turn, and rejection of a wrong parent digest before this card can close.

## Third boundary

The corrected-prefix retry reached the receipt scope checks but failed at
`prefixTokenCount`. The input-prefix digest now agrees; the Provider receipt and requester
checkpoint still carry different prefix counts. Raw output is retained under
`/tmp/spec182-r11-b4-probe-current/` (`requester.log`, `provider.log`). A temporary diagnostic
was added to report the two wire values, then will be removed after the native contract is
repaired.

## Fourth boundary

The terminal-role finalization repair executed the additional state-only ONNX Runtime epoch
and the Provider emitted the final stream cursor, but the requester did not receive a stream
completion callback before its 30-second request deadline. The raw run is retained under
`/tmp/spec182-r11-b4-finalize/` (`requester.log`, `provider.log`). The Provider log shows
application cursors `1..8` followed by cursor `9`; the requester accepted the encrypted
Response but remained pending, so no checkpoint or success marker was produced. This is now
the active boundary: determine whether the C++ stream consumer is missing the terminal cursor
or the conversation commit acknowledgement, then rerun the same process without changing the
fixture contract.

No `QUALIFICATION_PASS` claim is made by this record.

## Seventh boundary

The fresh-generation retry passed the first turn and the native coordinator accepted the
second turn identity, but Provider assembly failed at `DI_CANONICAL_ROOT_DIGEST_MISMATCH`.
The C++ publisher reused the same stable `assignedArtifact` name for both requests even
though each canonical root contains request-scoped source publication names. Provider's
process-wide artifact cache therefore served the first root for the second request. Raw
output is retained under `/tmp/spec182-r11-b4-fresh-generation/`. The publisher now adds
the canonical manifest digest to the stable artifact identity; a fresh rebuild and rerun
remain required.

No `QUALIFICATION_PASS` claim is made by this record.

## Fifth boundary

With C++ tracing enabled, the requester received the complete stream through terminal
cursor `9` and entered the conversation commit phase. The Provider decrypted and accepted
the authenticated `COMMIT` control, but the requester rejected the returned set with
`NATIVE_CONVERSATION_COMMIT_ACK_INCOMPLETE`. The Provider trace shows that its
`waitFor` loop reprocessed the same historical control records and published a growing
sequence of duplicate commit acknowledgements before the requester deadline. This is a
Provider C++ control-loop replay defect; it is not a Python wrapper failure. Raw output is
retained under `/tmp/spec182-r11-b4-trace/` (`requester.log`, `provider.log`). The next
retry will process each collaboration control sequence once, rebuild both native targets,
and rerun the two-round continuation plus wrong-parent negative.

No `QUALIFICATION_PASS` claim is made by this record.

## Sixth boundary

After the Provider control replay guard, the first C++ `FULL_CONTEXT` request completed,
persisted a native checkpoint, and emitted the stream oracle and success markers. The
second process reached the real Provider selection path but was rejected by the native
coordinator with `native conversation continuation requires a fresh stateful request`.
The continuation fixture reused the first turn's `generationId`; the C++ contract requires
a new request/generation identity while retaining the conversation parent checkpoint. Raw
output is retained under `/tmp/spec182-r11-b4-ackfix/`. The next retry will give the second
request a distinct generation identity, then re-run the two-round and wrong-parent checks.

No `QUALIFICATION_PASS` claim is made by this record.

## Final validation

After the publisher bound stable artifact identity to the canonical manifest digest, and the
static review changed decode-state retention to keep state only after successful terminal
promotion, a fresh build and independent process run completed both continuation turns. The first C++
`FULL_CONTEXT` request emitted the ordered stream oracle
`[4,5,6,7,8,9,10,2]`, real `COMMIT`/`FINALIZE`, a journal checkpoint, and
`NATIVE_REQUEST_SUCCEEDED`. The second request used a distinct generation identity and completed
`APPEND_DELTA` with `NATIVE_REQUEST_SUCCEEDED`; its plan digest differed from the first request,
and Provider emitted grant verification plus real CPU ONNX Runtime execution evidence for both
requests. A third process with a deliberately wrong parent checkpoint returned nonzero and was
rejected at the native conversation begin boundary (`NATIVE_CONVERSATION_BEGIN_FAILED` with
`DI_NATIVE_CONVERSATION_PARENT_MISMATCH`), with no success marker. The standalone driver now
asserts all positive and negative markers instead of reporting them only as informational output.

Raw run: `/tmp/spec182-r11-b4-final-checked/` (`requester.log`, `requester-second.log`,
`requester-wrong-parent.log`, `provider.log`). The final build used
`./waf build --targets=DI_NativeRequester,di-native-provider -j2` and passed in 54.362s.
The C++ selectors then passed: `unit-tests --run_test='Spec182*'` 256/256 cases and
7077/7077 assertions; `integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*'`
9/9 cases and 55/55 assertions. This closes R11-B4's bounded continuation process evidence;
it does not close the parent T010/T011 tasks or whole-Spec qualification.
