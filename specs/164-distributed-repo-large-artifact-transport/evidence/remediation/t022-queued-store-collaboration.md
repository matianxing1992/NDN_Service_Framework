# T022 Queued Store Collaboration

Date: 2026-07-30

## Corrected control semantics

The public ordinary-store path is:

```text
Request
→ advisory ACK offers
→ ACK_CLOSED
→ commit_plan
→ exact Selection assignment
→ Provider bounded execution queue
→ Response
```

A positive ACK reports `queueDepth`, `queueCapacity`, `availableBytes`, and
`maxArtifactBytes`. It does not create a capacity reservation, lease, pin, or
resource lock. Selection carries a task ID bound to the operation, artifact,
repository identity, and collaboration role. The Provider is configured with
one handler worker for the smoke, so accepted assignments enter a serialized
bounded execution queue.

The previous `ReplicaLeaseCollaborationClient` remains only as a compatibility
surface for the already-recorded pre-remediation experiments. The deployed
public adapter uses `ReplicaTaskCollaborationClient`.

## Protocol correction discovered by the smoke

Deferred collaboration planning previously transmitted application-owned
opaque assignment bytes without a separate framework-owned role field.
Providers therefore fell back to the service name during role authorization.
NDNSF now wraps each deferred assignment in a bounded TLV envelope containing
role and optional provisioning metadata while preserving the opaque
application bytes exactly. Legacy semicolon and arbitrary opaque assignments
remain accepted as legacy inputs; malformed new envelopes fail closed.

## Verification

- `GenericOpaqueSelection`: 8/8 native tests passed, including new-envelope
  byte identity and legacy non-envelope compatibility.
- Public collaboration backend Python contract: 1/1 passed.
- Artifact transfer Python contracts: 5/5 passed.
- Python syntax/import validation passed for the adapter, Provider/User smoke,
  and MiniNDN harness.

Successful real MiniNDN run:

```text
results/spec164-public-task-minindn-20260730T0734Z
```

Observed:

- Controller-authorized service and `artifact-replica-0` role;
- ACK message `store queue available`;
- ACK payload contains only advisory offer fields and no `leaseId`,
  `reservedBytes`, or `capacity reserved`;
- Provider lifecycle reaches `handler queued` and
  `collaboration handler running`;
- secure control verdict `PASS`;
- one Request plus one Selection: `controlOperationCount=2`;
- selected repository `/example/hello/provider/A`;
- 64 KiB network publication and cold fetch use 16 segments each;
- publication has zero timeout/retransmission;
- consumer authenticates the receipt and signed root;
- final destination is visible and byte-identical;
- top-level verdict `PASS`;
- `performanceClaim=false` because this is a functional integration smoke.

Evidence hashes:

```text
22e48024507c9a68dc423365f1357edefa95d21132ec679971da54abcb57ae01  summary.json
036f2614574013a4e2907fdc1f6cca991880f491749923430be3d83a6acc4272  control/summary.json
39ac28a014671424f490dd81328b07308bcac1015acb903357c0b24a5c8a7f54  repo-result.json
6241fb50bf023d92eaa4b9da52cdd061a45c015d3251e692d4864b92033fda3f  consumer-result.json
```

## Preserved diagnostics

- `...0731Z`: role envelope worked, but Controller policy lacked explicit role
  permission; corrected without bypass.
- `...0732Z`: secure control passed; 1 MiB functional fixture exceeded the
  smoke manifest graph bound.
- `...0733Z`: 64 KiB store reached ACTIVE, but consumer raced repository
  producer prefix registration and received a Nack; readiness was corrected
  with a bounded post-registration stabilization interval.

Transient identities, PIBs, private keys, receipt keys, and duplicate payload
bytes were deleted from all diagnostic and successful run directories.

## Boundary

This closes T022 only. It proves deployed public API/control integration and a
small segmented transfer. It does not prove bandwidth saturation, canonical
cold-read byte accounting, or the frozen performance success criteria; T023
and T024 own those claims.
