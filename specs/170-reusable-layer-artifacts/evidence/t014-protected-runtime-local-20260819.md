# T014 protected runtime local qualification (2026-08-19)

## Verdict

`PASS_LOCAL` for the T014 boundary. The corrected runtime now binds protected
dataflow authority, grant identity, group capability, keys/nonces, replay
fences, cancellation, and plaintext cleanup to the same sealed
request/attempt/plan/role/group/epoch/name identities. This is local protocol
and deterministic negative-test evidence; it is not a MiniNDN, exact-SIF,
CUDA, Qwen, or performance claim.

## Implemented boundary

- `GrantBindingV1` is distinct from the advisory `ProviderGrantViewV1` and
  binds the Provider grant name/digest to request, attempt, plan core,
  security-policy snapshot, and protection epoch.
- A protected Provider projection requires its exact local grant; plaintext
  projections remain wire-compatible and carry no null grant field.
- `GroupCapabilityV1` signs commitments to every member's wrapped epoch key,
  while each Provider projection discloses only that Provider's envelope.
- Operation keys and nonces bind the exact operation, producer, tensor and
  manifest digest, full Data name, and segment. An undeclared name, producer,
  consumer, role, rank, group epoch, or capability cannot authorize I/O.
- `ProtectedRuntime` revalidates the Selection, dataflow endpoints and peer
  roles, Provider boot/fencing state, grant, and group capability before the
  ONNX execution path. Revocation, cancellation, completion, and destruction
  drain device plaintext before host plaintext; cleanup failure remains
  fail-closed.
- `NdnsfCollaborationDependencyIo` applies the protected check before each
  publish/fetch operation. `NativeProviderHandler` refuses a protected
  Selection when no verified runtime factory is configured or when any exact
  binding differs.

The application-facing coordinator accepts an injected grant-binding provider,
and the native Provider accepts an injected protected-runtime factory. A live
NDN transport adapter that requests, verifies, decrypts, and refreshes grants
from `ArtifactPolicyAuthority` is not claimed by this local result; full
multi-Provider wiring belongs to the subsequent integration gates.

## Reproduction

```bash
./waf build -j4
./build/unit-tests --log_level=message

PYTHONPATH=NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q --tb=short tests/python/test_spec170_*.py

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ExactTensorDataUsesSignedConsumerPullAndSameNameRetry \
  --log_level=message

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/V3DependencyIoPublishesManifestThenReconstructsExactSegments \
  --log_level=message
```

Observed results:

- full Waf build: 486/486 build steps, success;
- C++ unit tests: 525 test cases, no errors;
- Spec170 Python: 145 passed, 10 skipped, one unrelated historical
  `torch.load` warning;
- both focused production-codec/handler integration tests: no errors.

## Remaining boundary

T015 must exercise the full four-Provider/four-role lifecycle in one reusable
integration environment. T016 must add deterministic packet drop, reorder,
duplicate, repair, replay, and cancel cases. Neither this result nor the two
focused integration tests authorize a distributed-success or hardware claim.
