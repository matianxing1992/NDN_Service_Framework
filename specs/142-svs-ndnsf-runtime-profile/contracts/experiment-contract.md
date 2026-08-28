# Spec 142 Experiment Contract

## Frozen claim

Compare `publicationPreparationWorkers=0` and `1` under the effective current
NDNSF NDN-SVS runtime profile. Do not compare V2 against V3, one-byte against
normal piggyback, or different Fetch-recovery policies.

## Fixed configuration

```text
nodes                         = [peer-a, peer-b]
traffic                       = bidirectional publish + subscribe
link_bandwidth                = 100 Mbps
link_one_way_delay            = 10 ms
configured_loss               = 0%
cpu_affinity                  = 0-3

protocol                      = V3
sync_interest_lifetime        = 1000 ms
sync_suppression              = 1 ms
periodic_sync                 = 30000 ms
use_timestamp                 = false
application_payload           = 256 bytes
effective_max_piggy_data      = 800 bytes

publication_fetch_window      = clamp(ceil(target_pps * 0.64), 32, 128)
mapping_fetch_window          = 10
mapping_fetch_retries         = 0
mapping_fetch_failure_backoff = 200 ms
publication_fetch_retries     = 2
publication_fetch_inner_retry = 2
publication_fetch_lifetime    = 500 ms
publication_fetch_lifetime_bounds = 250-2000 ms
publication_fetch_backoff     = 50 ms
publication_fetch_max_backoff = 2000 ms

parallel_sync_processing      = 4 workers, queue 256
parallel_sync_production      = 4 workers, queue 256
sync_signing_in_worker        = false
sync_extra_block_in_worker    = true
sync_batching                 = false

publication_security          = RSA-2048 sign + validate
pacer                         = independent high-resolution APP pacer per peer
timing                        = 10 s warmup / 60 s measure / 10 s drain
formal_cell_retries           = 0
```

The runner MUST print the resolved value for every field. A requested or
exported environment value is not evidence of an effective value.

## Sole treatment

```text
face-inline-rsa:
  publicationPreparationWorkers = 0

worker-rsa:
  publicationPreparationWorkers = 1
```

Queue capacity, FIFO ordering, Face commit path, binary, libraries, topology,
workload, and all other options are identical.

## Admission and execution

1. Freeze one build/runtime manifest and reject installed/workspace library
   mixing.
2. Prove the two independent pacers can each attempt the maximum requested rate
   within +/-2% without making that pacer-only check a formal network cell.
3. Run exactly one 400-inline and one 400-worker cell.
4. Require both 400 receipts to pass every profile/harness invariant.
5. Only then run exactly one cell for each mode at 600 and 800 pps.
6. Never replace, tune, or selectively repeat a started formal cell.

## Fetch-health invariant

For a clean worker-only comparison, each peer MUST report the following
counter deltas during the 60-second measurement window (`measureEnd -
measureStart`):

```text
mapping_retry_count             = 0
mapping_timeout_count           = 0
mapping_nack_count              = 0
publication_retry_count         = 0
publication_timeout_count       = 0
publication_nack_count          = 0
```

Mapping and publication Fetch Interests/Data may be nonzero when normal
piggybacking does not carry a publication; these are reported. Recovery
activation is not silently discarded: it makes the receipt `PROFILE_INVALID`
for the causal comparison and is preserved as boundary evidence.

Warmup and drain counters remain in the peer summary as diagnostic evidence,
but they do not contaminate the measurement-window invariant.

## V3 security semantics

The outer V3 Sync Interest is not separately signed like the V2 Interest.
Its ApplicationParameters contain an RSA-signed state-vector envelope Data
packet, and the parameters digest binds that envelope to the Interest. Evidence
therefore requires `syncEnvelopeSignatureType=RSA` and validation of the
embedded Data; it must not mislabel the outer Interest as RSA-signed.

## Required metrics

Per peer and aggregate:

```text
attempted_count, attempted_pps, pacing_error_pct
delivered_count, delivered_pps, delivery_ratio
delivery_latency_mean_us
delivery_latency_p50_us
delivery_latency_p95_us
delivery_latency_p99_us
signed_publication_wire_bytes
piggyback_eligible_count, piggyback_hit_count
mapping_interest/data/retry/timeout/nack counts
publication_interest/data/retry/timeout/nack counts
RSA sign/validate counts and failures
CPU time/utilization, thread count, queue high-water marks
```

All latency statistics and delivery counts MUST be recomputed from hashed raw
samples. Partial-delivery percentiles are labeled survivor distributions.

## Claim restrictions

- Spec 140/141 data cannot be pooled into Spec 142.
- A `PROFILE_INVALID` cell cannot support a publication-worker performance
  claim.
- The experiment may show that worker placement removes Face blocking; it
  cannot claim RSA itself became faster.
- The experiment is a two-node NDN-SVS microbenchmark under NDNSF-effective
  settings, not a full NDNSF application-workflow benchmark.
