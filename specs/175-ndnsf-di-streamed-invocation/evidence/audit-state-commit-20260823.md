# Spec175 audit correction: admission-ordered state and stream terminal

Date: 2026-08-23

## Scope

This is a focused regression correction, not a G1/G2 qualification result.

## Corrections

1. `OnePlanGenerationLoop` now previews the candidate tokenizer delta and
   accepted-prefix digest, publishes the external event, and only then commits
   the tokenizer/prefix state. A Core rejection therefore cannot leave a token
   visible to a retry or replacement attempt.
2. The DI `TerminalAwareContext` releases a selection reservation only after
   the terminal publication returns successfully. Rejected/fenced terminal
   writes remain attributable to the handler/failure path.
3. Native `CollaborationContext::publishFinalResponse` detects a streamed
   request and closes it through `finishStream(ApplicationComplete)`, so a
   legacy native handler cannot silently bypass the End event.
4. `finishStream` and `failStream` commit the collaboration terminal only
   after `StreamEventPublisher` accepts the End/failure. A bounded rejection
   can therefore still produce the appropriate failure path.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  pytest -q tests/python/test_streamed_invocation_api.py \
    tests/python/test_spec175_streamed_generation.py
18 passed

focused Spec175 Python aggregate
57 passed

./waf build --target=integration-tests -j2
PASS

build/integration-tests --run_test=Spec175InvocationStream --log_level=test_suite
14/14 PASS
```

The result does not register any formal G2 I01-I15 case. Native multi-role
NDNSF-DI, fault matrix, exact SIF, MiniNDN, and Tiger evidence remain open.
