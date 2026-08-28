# Spec 149 Post-Implementation Audit

**Verdict: BLOCK**

## What passed

- Core recovery metadata is no longer counted as Mapping.
- One frontier fetch is shared and exact group-name fetches are coalesced.
- Verified frontier/group metadata is cached and retention-pruned.
- Recovery-control status is exposed in C++, Python, and UAV structured logs.
- Full 367-target build passed.
- All 394 native tests passed.
- Focused Python tests passed 7/7.
- Core contains no workload-specific recovery branch.
- The immutable zero-loss formal cell passed at 99.7793% delivery.

## Blocking evidence

The immutable 1% loss/1% reorder cell delivered only 1.4901%. FEC was active:
1,267 attempts produced 474 validated recoveries. Recovery control was bounded
relative to the prior failure (2,314 controls and zero Mapping Interests), but
ordered application delivery stopped after cursor 399.

SC-001 is not fully satisfied because a deterministic terminal-gap,
multi-waiter failure/progress test is still missing. SC-005 fails because
impaired delivery is below 98%. T002 and T006 remain open.

## Required successor

Do not modify or rerun Spec 149. Define a new Spec that:

1. serializes ordered drain ownership;
2. guarantees a terminal-gap insertion wakes or joins the drain;
3. discards/classifies recovery arriving behind the delivery cursor;
4. exposes next-delivery cursor, ready depth/oldest cursor, terminal-gap depth,
   and stale-recovery counters;
5. proves the boundary deterministically before one fresh MiniNDN campaign.
