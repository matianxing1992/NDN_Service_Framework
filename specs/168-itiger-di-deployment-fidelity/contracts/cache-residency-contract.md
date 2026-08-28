# Model Artifact Cache and Residency Contract

## Identity

A cache key is not a model name. It is the digest of model content, tokenizer,
semantics/adapter, dependency graph, partition, precision, runtime backend, and
artifact bytes. Every claimed hit includes Provider boot epoch and storage/device
identity.

## Residency tiers

| Tier | Required evidence | Reuse effect |
|---|---|---|
| `DISK` | content-addressed path, size, full digest, durable commit | skip Repo transfer |
| `HOST_MEMORY` | live object/lease over verified bytes, layout identity | skip disk read/deserialize where supported |
| `GPU` | live adapter session/tensors, assigned device, bytes, runtime/layout identity | skip transfer and device load |

File existence, a per-request copy, `LOADING` progress, or stale ACK metadata is
not GPU residency.

## ACK advertisement

Providers advertise a bounded list or digest-addressable inventory containing:

```text
artifact/model identity
partition and role compatibility
tier
bytes
device
verified_at / last_used_at / expires_at
provider boot epoch
pin or eviction eligibility
```

ACK processing is read-only. It does not reserve a GPU, fetch bytes, or evict an
existing model.

## Planning preference

For each feasible graph partition, the default strategy minimizes expected cold
work after enforcing correctness constraints:

1. compatible GPU-resident assignment;
2. compatible host-memory assignment;
3. compatible verified-disk assignment;
4. already published artifact fetched to a feasible Provider;
5. newly generated and published partition.

RTT, usable bandwidth, queue/load, and capacity may change ordering inside a
tier. A residency preference never overrides GPU capacity, graph correctness,
security domain, boot epoch, deadline, or artifact availability.

## Preparation behavior

- Missing data is fetched through DistributedRepo and verified before visibility.
- Partial/corrupt content is never promoted to `VERIFIED_DISK`.
- The content-addressed object is the payload owner. A request work directory
  uses a link, descriptor, mapped view, or adapter-supported reference whenever
  possible; it must not copy multi-GB payloads by default.
- Host/GPU promotion emits begin, progress, completion, and failure records.
- `GPU_RESIDENT` is published only after adapter validation on the assigned CUDA
  device and before `LOCAL_READY`.

## Warm-request acceptance

A compatible repeated request must show:

```text
duplicate_model_payload_bytes == 0
redundant_repo_segments == 0
redundant_device_loads == 0
false_gpu_hits == 0
```

The plan must retain the ACK inventory entry and strategy decision that caused
reuse. A lower total latency alone is not proof.

## Invalidation and eviction

- Provider restart invalidates RAM/GPU records via boot epoch.
- Identity mismatch invalidates the hit but need not delete the unrelated object.
- CUDA/context/device loss immediately invalidates GPU residency.
- Eviction is policy-driven and reported; request cleanup does not evict a
  compatible reusable shard merely because that request completed.
- Loading a conflicting model may evict an unpinned resident shard, but the
  transition and subsequent ACK inventory must reflect the new state.
