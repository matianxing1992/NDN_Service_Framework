# Ownership And Security Contract v1

## Placement Ownership

For one committed attempt:

- number of selected Providers equals number of execution roles;
- every role appears exactly once;
- every selected Provider appears exactly once;
- a role never spans Provider identities;
- an unselected Provider owns no execution authority;
- a planner proposal has no authority until trusted-core commit and Provider-specific Selection.

Tensor parallelism follows the same rule by representing ranks as complete execution roles:

```text
TensorGroup(Stage1, worldSize=2)
  Stage1/rank0 -> Provider P1
  Stage1/rank1 -> Provider P2
```

The group defines synchronization and failure atomicity. Neither rank can be replaced, selected, or failed over independently inside the same group epoch.

## Authority Layers

| Layer | Grants | Does not grant |
|---|---|---|
| Permission/NAC-ABE | access to service-level request/response data under policy | role assignment or device execution |
| UserToken | fresh request binding | Provider execution |
| ProviderToken | Selection/targeted handshake binding | arbitrary role/model access |
| signed ACK | authenticated willingness/capability evidence | execution authority |
| committed plan | global legal assignment and contracts | local execution until projected/verified |
| Provider Selection/grant | exactly one local role and dataflow for one plan/epoch | another role/Provider/plan |
| group capability | exactly one rank membership/schedule/local key projection | arbitrary collective or cross-rank key access |
| local admission lease | bounded runtime/device capacity | authorization beyond its exact bindings |

All applicable layers must pass; one layer cannot substitute for another.

## Exact Provider Grant Binding

The Provider grant binds at minimum:

- requester, invocation/request, and attempt;
- ACK closure and policy snapshot digests;
- plan identity/digest and generation;
- Provider identity and current boot/session epoch;
- exactly one role/rank and graph/artifact assembly identities;
- incoming/outgoing dataflow contracts;
- group/epoch/membership/schedule where relevant;
- result contract, deadline/expiry, and protection epoch;
- normal tokens and signature/issuer trust.

The Provider verifies the grant against the received Selection and local active state before artifact decryption, preparation that consumes protected resources, or execution.

## Exact Group Capability Binding

The local group capability binds:

- plan/group/epoch and complete membership digest;
- local Provider, role, rank, world size;
- ordered collective operations/rounds and exact input/output identities;
- artifact/dataflow/policy/protection identities;
- local wrapped-key commitment and expiry.

Capabilities are Provider-local and non-transferable. A capability for rank 0 cannot authorize rank 1, a new round, a new group epoch, or another plan.

## Protected Runtime Sequence

```text
verify normal NDNSF security
verify Selection and Provider grant
verify group capability when applicable
verify current boot/session, policy, attempt, plan, role/rank, epoch
verify artifact/dataflow identities
atomically revalidate/acquire local resource lease
unwrap only the local required key/material
create ORT inputs/session under the exact lease
execute
release plaintext/runtime references
zeroize key/plaintext buffers
record non-secret terminal evidence
```

No debug flag, missing field default, cache hit, or previously loaded session may bypass this sequence. Cache reuse is allowed only when exact compatibility evidence proves the same required authority and accepted semantics.

## Replay And Stale-State Rules

- one-time tokens cannot be reused for a different request or Selection;
- old Provider boot/session epochs are invalid after restart;
- a new attempt/plan/generation/group epoch fences old Data and capabilities;
- same-name Data with different bytes is rejected;
- late output from a cancelled/superseded attempt is not accepted by default;
- an old cached session/key/plaintext object cannot be rebound by shape/model label alone;
- replan produces new Provider grants/capabilities; it never edits old ones.

## Resource Admission

An ACK advertises a sanitized snapshot, not a reservation. Immediately before execution the Provider atomically validates a full peak vector including model/initializer/session, activation, KV/cache, collective workspace, transfer/reassembly, provider/runtime overhead, device and host memory, and configured safety margin. Admission failure is bounded and cannot silently fall back to an unapproved device.

## Zeroization And Release

The implementation must release/zeroize on:

- success;
- model/runtime exception;
- dependency failure or timeout;
- cancellation;
- replan/supersession;
- group failure;
- Provider shutdown/restart;
- local admission revocation.

Tests must observe terminal zeroization acknowledgements or equivalent verifiable state without exposing the protected bytes.

## Mandatory Rejections

- duplicate/missing Provider-role assignment;
- wrong requester/request/attempt/closure/policy/plan/generation;
- wrong Provider/boot epoch/role/rank/group/epoch/world size;
- wrong artifact/dataflow/result contract;
- wrong/expired/replayed token, grant, capability, or signature;
- wrapped-key commitment mismatch;
- incomplete peak vector or changed resources at admission;
- cross-tenant cache/session reuse without exact evidence;
- execution after cancel/replan/restart;
- CPU fallback in a GPU-required acceptance case.

## Evidence Boundary

Record identities, digests, validation outcomes, resource-vector category totals, lease state, and zeroization status. Never record raw tokens, private keys, unwrapped keys, plaintext model parameters, activations, user inputs, or mutable handles.
