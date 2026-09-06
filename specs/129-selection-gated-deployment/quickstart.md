# Quickstart Validation: Spec 129 R1

This guide is a validation contract, not evidence that R1 is implemented.
Do not run the live matrix until every deterministic gate and the
pre-implementation audit pass. Never reuse R0 results.

## Prerequisites

```bash
git status --short
codegraph status .
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/129-selection-gated-deployment --strict
```

Confirm `.specify/feature.json` identifies Spec 129 and record immutable Spec
128 hashes. Resolve and stop any other Spec 129 campaign process before live
execution; use one unique output directory and process-level single writer.

## Focused R1 deterministic gates

Run the focused C++ tests that cover:

- ReservationLease/SelectionDecision/receipt/encrypted projection codecs plus
  non-reserving SelectionInputKeyOffer and input-only/DI-bound grant codecs;
- Provider-targeted V2 name parse/round trip;
- ACK deadline closure, late ACK tombstone, duplicate/reordered decisions;
- recipient encryption, wrong certificate/AAD/name/replay/plaintext negatives;
- REQUEST input ciphertext/reference, absence of input key before Selection,
  selected-role input-key grant, and unauthorized/downstream-role denial;
- existing token, Provider permission, replay, NAC-ABE and generic compatibility.
- four capability combinations and fail-before-publish for gated-input Targeted,
  while ordinary Targeted remains unchanged.

Run the focused Python tests that cover:

- atomic reserve before positive ACK, negative ACK zero allocation and quotas;
- lease expiry, exact release cause, restart and journal recovery;
- global plan plus minimum per-Provider projections;
- local DAG eligibility, pipeline overlap and abort propagation;
- bounded full-jitter retry/exhaustion with no fairness claim;
- C++/Python parity and pull-only status.

Planned focused commands (the new tests must exist before this gate is run):

```bash
for suite in GenericDynamicApi/DeploymentControl GenericDynamicApi/CryptoAndAuthorization GenericDynamicApi/SelectionStrategies GenericDynamicApi/CollaborationStatus GenericDynamicApi/TargetedInvocation DistributedExecutionConsistency; do
  ./build/unit-tests --run_test="$suite/*" --log_level=message || exit 1
done
python3 -m unittest discover -s tests/python -p 'test_spec129_*.py'
```

The focused set must include non-DI positive-ACK/no-reservation compatibility,
authorization-before-reserve, conflicting immutable decisions, commit at/after
tentative expiry, release-before-retry, input-key erasure, and no plaintext-key
persistence. Passing obsolete R0 barrier tests is not R1 acceptance.

## Compatibility and security gates

```bash
examples/run_hello_auth_regression.sh
examples/run_hello_ack_payload_regression.sh
examples/run_selective_ack_custom_selection_regression.sh
examples/run_nac_abe_attribute_routing_regression.sh
examples/run_token_handshake_negative_regression.sh
```

Add `examples/run_targeted_selection_confidentiality_regression.sh` before the
live matrix. Packet/content scan must find zero plaintext exact assignments or
input for negotiated `SelectionGatedInputV1` calls and must confirm unchanged
input behavior when that capability is absent.

## Full build and binding rebuild

Use the repository's normal full Waf build, full C++ suite, forced in-place
Python binding/package rebuild, and full focused/general Python suites. Record
commands, counts, source identity, build identity and failures. Do not claim
parity from import-only tests.

## Fresh MiniNDN confirmation

The frozen manifest contains exactly the twelve plan.md scenarios. The runner
must reject duplicate cell IDs, missing cells, reused outputs, source drift,
automatic retry, incomplete attribution, concurrent writers, plaintext exact
assignment/input, or Spec 128 hash drift before launching a live cell.

Required aggregate fields include:

```text
cells planned/completed/unique
positive/negative/late ACKs
reservations created/committed/released/orphaned
release cause and latency
SELECTED/NOT_SELECTED/receipt/retry/timeout/Nack counts
reservation amplification and hold time
contention collision/backoff/exhaustion
stage start/end, overlap, abort and terminal completion
Payload and control bytes
p50/p95 end-to-end latency
automaticRetry=false
Spec 128 hashes before/after
```

Every cell runs at most once. Preserve a failed cell and close R1 as measured
negative if any frozen correctness gate fails.

## Closeout

After the complete fresh matrix, run Spec Kit analyze and post-implementation
audit. Update traceability with exact test/evidence paths. R1 can close only if
all functional requirements and success criteria have direct evidence; R0
ReadySet/ExecutionActivate results cannot substitute.
