# T033 — Remove ACK-Time Capacity Reservation

## Decision

Repository storage uses advisory ACK offers and Selection-bound task
assignment. A positive ACK does not reserve bytes, create a lease, or lock
storage. After Selection, the Provider admits the idempotent operation to its
bounded execution path; definitive capacity and concurrency checks occur when
that task executes.

`commit_plan`, when a generic collaboration carrier uses it, binds an immutable
ACK-closed snapshot to exact assignments. It is not a resource reservation.
The ordinary repository public API does not expose a separate plan-commit or
capacity-reservation call.

## Removed active behavior

- `NetworkDistributedRepoClient.enable_capacity_reservations`
- public `reserve_capacity()` and `release_capacity()` helpers
- client `_reserve_replicas()` / `_release_reservations_parallel()` control
  rounds
- Provider `RESERVE_CAPACITY` / `RELEASE_CAPACITY` handlers and versioned
  service names
- reservation accounting in capability ACKs, successful commits, and failure
  cleanup

The old `capacity_reservations` SQLite table remains readable for additive
schema migration and rollback. Current runtime paths neither consult nor
mutate it. Likewise, the legacy artifact lifecycle spelling `RESERVED` remains
decode-only compatibility; new assignments enter `QUEUED`.

## Verification

Executed from the repository root with:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_ha.py -v

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec164_*.py' -v
```

Results:

- repository HA/orchestration tests: 48/48 pass;
- Spec 164 tests: 102/102 pass;
- the legacy-table test proves a historical `RESERVED` row does not reduce
  advertised free bytes and is not consumed by a current write;
- RF=3 `QUORUM` and `ALL` tests now exercise direct store assignments and
  receipt thresholds without a reserve/release round;
- versioned service tests reject both retired reservation operations.

The canonical fourth MiniNDN campaign remains the throughput authority because
it exercised the queued artifact path that already excluded ACK-time
reservation. Removing the disconnected legacy round does not reinterpret or
replace any retained measurement.
