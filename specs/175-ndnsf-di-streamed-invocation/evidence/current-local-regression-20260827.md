# Spec175 current local regression — 2026-08-27

## Scope

This record covers the post-fix local regression only. It is not a G0–G4
qualification manifest and does not claim MiniNDN, SIF, CUDA, or Tiger
completion.

## Defects found and fixed

1. The four-Provider production-ingress test treated asynchronous ACK vector
   order as Provider identity. Selection role assignments are now keyed by the
   candidate's Provider name, so a different ACK arrival order cannot swap
   roles.
2. The same test dereferenced a missing `roleProviders` iterator after a failed
   assertion, turning a useful mapping failure into a segmentation fault. The
   assertion now returns safely after reporting the missing mapping.
3. The candidate-only `_v3_role_specs(candidate)` compatibility call remains
   valid without fabricating graph I/O contracts; production certification
   still receives the explicit post-ACK graph.
4. A V3 topology uses an empty device tuple for CPU. A literal `"cpu"` device
   identity is rejected; CUDA identities remain `cuda:<index>`.
5. ConversationStateStore now wipes retained TensorBundle payload bytes before
   eviction, expiry cleanup, Provider-boot invalidation, and explicit clear;
   the native conversation-store regression covers the cleanup boundary.
6. Provider collaboration reception now drops reserved `user-control-v1`
   records addressed to another Provider, without changing the requester-name
   convention for ordinary Provider-to-Provider data.
7. Generation lineage now authenticates a closed transition kind
   (`PREFILL`, `DECODE`, or `CHECKPOINT_FINALIZE`) and rejects inconsistent
   epoch/kind combinations. This prevents a terminal state-only pass from
   being accepted as an ordinary decode transition.
8. The Provider worker could previously invoke a complete `runStreamed()`
   loop once for every lineage-bearing coordinator epoch. Coordinator-owned
   epochs now use one-shot `run()`; standalone streamed calls remain available
   only without authenticated coordinator lineage, and the ONNX adapter rejects
   the ambiguous combination.
9. Conversation continuation restored the promoted parent state but reported
   zero avoided prefix work at request epoch zero. The coordinator now records
   the exact parent prefix length while keeping `conversationStateHit` separate
   from the request-local `decodeStateHit` metric.

## Verification

```text
PYTHONPATH=. pytest -q tests/python/test_spec175_*.py \
  tests/python/test_spec168_provider_generation.py
195 passed in the focused Spec175 + Spec168 provider-generation regression.

PYTHONPATH=. pytest -q tests/python/test_spec175_*.py \
  tests/python/test_spec168_provider_generation.py tests/python/test_spec170_*.py
349 passed, 10 skipped, 1 warning in the broader Spec175/168/170 regression.
Both are regression evidence, not a formal qualification gate.
One existing `torch.load(weights_only=False)` FutureWarning is emitted by the
historical Spec170 content-addressed reuse test; it is not a Spec175 failure.

./build/unit-tests --log_level=error
600 test cases; *** No errors detected

./build/integration-tests --run_test=\
  'Spec170NdnsfDiCoreFlow/ProductionIngressRunsFourProviderRoleSplitRequestSelectionResponse' \
  --log_level=message
1 test case; *** No errors detected (three independent process repeats)

./build/integration-tests --log_level=error
full suite; *** No errors detected

./build/unit-tests --run_test='*ConversationState*' --log_level=error
6 test cases; *** No errors detected

The lineage transition-kind and coordinator-owned runner-boundary regressions
are included in the 600-case native unit run.

The conversation-continuation regression now reports three avoided parent
tokens for the three-token promoted prefix and remains covered by the full
600-case native unit run.
```

## Follow-up after the 2026-08-27 source fix

The shared Spec168 provider-generation regression exposed an indentation bug in
`_qwen_generation_spec()`: an envelope without a conversation block returned
`None` even when its validated mode was `TOKEN_STREAMING`. The return object is
now emitted for both the ordinary and conversation paths; the existing
`useCache=false` rejection remains unchanged.

The following gate invocations replayed the historical diagnostic seal shown
above; they are not current-source qualification because later source/docs
edits changed the subject. T020 must regenerate them after implementation
closure:

```text
PYTHONPATH=. pytest -q tests/python/test_spec168_provider_generation.py
15 passed

PYTHONPATH=. pytest -q tests/python/test_spec175_*.py \
  tests/python/test_spec168_provider_generation.py
195 passed in the focused Spec175 + Spec168 provider-generation regression.

PYTHONPATH=. pytest -q tests/python/test_spec175_*.py \
  tests/python/test_spec168_provider_generation.py tests/python/test_spec170_*.py
349 passed, 10 skipped, 1 warning in the broader Spec175/168/170 regression.
Both are regression evidence, not a formal qualification gate.

python3 scripts/run_spec175_python_gate.py \
  --output /tmp/spec175-python-gate-current.json \
  --source-seal results/spec175/g0/source-seal-current-20260827.json
historical sealed subject: 197 passed, 0 failed, 0 skipped (19 registered targets)

python3 scripts/run_spec175_integration_gate.py \
  --binary ./build/integration-tests \
  --source-seal results/spec175/g0/source-seal-current-20260827.json \
  --output /tmp/spec175-integration-gate-current.json \
  --healthy-repeats 1
historical sealed subject: 20/20 registered native integration cases passed

./build/unit-tests --log_level=error
600 test cases; *** No errors detected
```

The launcher safety test remains 35/35. The current shell is not privileged,
so a new real MiniNDN M11--M14 run still cannot start (`Mininet must run as
root`); this follow-up therefore does not close T022/T033 or any SIF/Tiger/
CUDA gate.

The first full integration run before the test fix failed with five mapping
assertions and a segmentation fault. The rebuilt current binary passes the
focused case and the complete integration suite.

## Root MiniNDN M01 correction probe

After the CPU V3 topology correction, one fresh root MiniNDN process completed
the real four-Provider tiny-ONNX path:

```text
case=M01 seed=1750007
NDNSF_DI_SPEC175_MININDN_WRAPPER_PASS
result status=PASS userReturnCode=0 providerCount=4
generated tokens=4,5,6,7,8,9,10,2 events=8 retries=12 duplicates=0
distributed latency=6562.13 ms
```

The result is preserved at `/tmp/spec175-m01-root7.2DtrHZ` with the case-result
manifest and per-node logs. It is a real host/CPU execution probe, not a G3
qualification result: M02--M14, repeated-process requirements, source sealing,
and the later SIF/Tiger gates remain open.

The post-guard M01 smoke used a new root process and completed cleanly:

```text
case=M01 seed=1750008
result=/tmp/spec175-m01-socket-2l4uY3/spec175-case-result.json
status=PASS userReturnCode=0 providerCount=4
generated tokens=4,5,6,7,8,9,10,2 events=8 retries=13 duplicates=0
distributed latency=7944.83 ms
```

This confirms the stale-socket guard preserves the real four-Provider path; it
is still a smoke probe and does not expand the G3 matrix.

## Launcher stale-socket guard

After `Minindn.cleanUp()`, the Spec175 runner calls
`cleanup_unused_nfd_sockets()` with only the controller, user, repository, and
four provider node names. A socket is unlinked only after an AF_UNIX connect
returns `ECONNREFUSED`/`ENOENT`; a successful connection or an indeterminate
error aborts startup. The focused launcher regression covers both an unowned
stale socket and an active listener (35 cases total), preventing a previous
run's NFD control path from being treated as readiness.
