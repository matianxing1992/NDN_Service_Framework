# Spec 175 audit rerun (2026-08-23)

> Historical rerun captured before native I01 registration. See
> `evidence/t014-native-i01-20260823.md` for the current I01 process evidence;
> this file preserves the earlier all-missing registry snapshot.

This rerun checked the active implementation and the formal gate entry points
after the state/terminal ordering and streamed-cache corrections. It is an
audit checkpoint, not a promotion result.

## Commands and results

```text
python3 scripts/spec175_contract_gate.py \
  --feature-dir specs/175-ndnsf-di-streamed-invocation \
  --output /tmp/spec175-contract-audit.json
  -> BLOCKED (DIRTY_INPUT_TREE; 5028 dirty paths in this rerun)

python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests --cases I01-I15 \
  --healthy-repeats 3 --seed 1750001 \
  --output /tmp/spec175-g2-audit.json
  -> BLOCKED_MISSING_CASES (I01-I15; 15/15 absent from the formal registry)

./waf build --target=unit-tests -j2
  -> PASS

./build/unit-tests --log_level=message
  -> PASS (562/562)

./build/integration-tests --log_level=message
  -> PASS (53/53); repeated in three fresh processes, all PASS

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_streamed_invocation_api.py \
  tests/python/test_spec175_cpu_fixture.py \
  tests/python/test_spec175_evidence.py \
  tests/python/test_spec175_integration_gate.py \
  tests/python/test_spec175_qwen_stateful_onnx.py \
  tests/python/test_spec175_streamed_generation.py \
  tests/python/test_spec175_qwen_generation.py \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_onnx_deployment_boundary.py
  -> PASS (57 passed)

./build/unit-tests --run_test=ProviderRoleWorkerDoesNotCacheStreamEventSideEffects,ProviderRoleWorkerPassesStreamEventSinkToNativeRunner --log_level=message
  -> PASS (2/2)

./build/integration-tests \
  --run_test=Spec175InvocationStream/NativeTinyOnnxAdapterRunsIncrementalStream \
  --log_level=message
  -> PASS (8 exact token events, EOS, final payload)
```

The missing-case result is intentional fail-closed behavior. The registry is
not populated with the existing generic one-Provider stream tests because those
tests do not satisfy the validation contract's tiny-ONNX Provider/role identity,
ACK-driven placement, or activation/feedback lineage requirements.

The existing development regressions remain the evidence boundary recorded in
`audit.md`: full native unit 562/562, full integration 53/53,
`Spec175InvocationStream` 14/14, and the focused Spec175 Python aggregate 57/57.
They establish transport and stateful-ORT prerequisites only. G1/G2 and every
SIF/MiniNDN/Tiger gate remain blocked until conforming cases and sealed
manifests exist.

The lifecycle correction also rebuilt the integration target successfully and
reran the focused native two-role streamed deferred-collaboration prerequisite
plus `Spec175InvocationStream` (14/14). The prerequisite now proves explicit
non-final-role cleanup and one terminal owner with the deterministic runner;
it is still not a formal tiny-ONNX I-case and is therefore not registered in
G2. The streamed terminal-role exact-forward cache is now bypassed whenever an
event sink is present; otherwise a cached TensorBundle result could suppress
the externally visible event side effect on retry. The dedicated unit case
proves two runner executions and two emitted events.

The native C++ adapter now has a separate incremental tiny-ONNX regression. It
proves one persistent ORT session, state feedback across eight epochs, exact
greedy token IDs, EOS termination, and one final payload. It remains below G2:
the formal registry must name a native multi-Provider NDNSF-DI test that
includes Request/ACK/plan/Selection and activation/feedback lineage.

The full unit run exposed a scheduling-sensitive assertion in the existing
`StreamFacade/PredictiveTerminalGapAdvancesOrderedDrain` test: a bounded
future terminal-gap marker could remain after `nextDeliverCursor` advanced.
The test now checks the signed predictive horizon bound instead of requiring a
racy zero-depth snapshot. No production stream behavior was changed; the
post-fix full unit and integration binaries both pass.
