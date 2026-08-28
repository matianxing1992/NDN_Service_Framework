# Spec 143 Diagnostic Contract

## Frozen baseline

```text
baseline_spec        = 142
baseline_campaign    = campaign-20260724T012559Z
baseline_mutation    = forbidden
baseline_rerun       = forbidden
```

## Fixed cell

```text
nodes                         = [peer-a, peer-b]
traffic                       = bidirectional publish + subscribe
rate                          = 400 pps per peer
mode                          = worker-rsa
publication_workers           = 1
link_bandwidth                = 100 Mbps
link_one_way_delay            = 10 ms
configured_loss               = 0%
cpu_affinity                  = 0-3
timing                        = 10 s warmup / 60 s measure / 10 s drain
cell_retries                  = 0

protocol                      = V3
sync_interest_lifetime        = 1000 ms
sync_suppression              = 1 ms
periodic_sync                 = 30000 ms
use_timestamp                 = false
application_payload           = 256 bytes
effective_max_piggy_data      = 800 bytes
publication_fetch_window      = 128
mapping_fetch_window          = 10
mapping_fetch_retries         = 0
mapping_fetch_failure_backoff = 200 ms
publication_fetch_retries     = 2
publication_fetch_inner_retry = 2
publication_fetch_lifetime    = 500 ms
publication_fetch_bounds      = 250-2000 ms
publication_fetch_backoff     = 50 ms
publication_fetch_max_backoff = 2000 ms
publication_security          = RSA-2048 sign + validate
```

All remaining parallel Sync settings and queue capacities equal the Spec 142
r4 manifest. A resolved-profile mismatch stops before the cell starts.

## Required structured events

```text
fetcher_queued
fetcher_dispatched
fetcher_data
fetcher_nack
fetcher_timeout
fetcher_validation_start
fetcher_validation_success
fetcher_validation_failure

producer_interest
producer_store_hit
producer_store_miss
producer_data_put
mapping_producer_interest
mapping_producer_empty
mapping_producer_data_put

publication_fetch_queued
publication_fetch_dispatch
publication_fetch_success
publication_fetch_retry_scheduled
publication_fetch_expired
piggyback_cache_satisfy

mapping_fetch_dispatch
mapping_fetch_data
mapping_fetch_nack
mapping_fetch_timeout
```

Each Interest event includes `event=`, `name=`, `nonce=`, and an attempt ID
where the owning layer has one. Semantic SVSPubSub events include the full
publication key. No payload, private key, certificate private material, or
application secret is logged. Logs use:

```text
*=WARN:
ndn_svs.Fetcher=TRACE:
ndn_svs.SVSyncBase=TRACE:
ndn_svs.MappingProvider=TRACE:
ndn_svs.SVSPubSub=TRACE
```

The actual environment string is preserved in `commands.json`.

## Classification gate

```text
if timeout_count == 0:
    status = INCONCLUSIVE
elif classified_timeout_count / timeout_count < 0.95:
    status = INCONCLUSIVE
else:
    status = DIAGNOSED
```

`UNCLASSIFIED` does not count as classified. No classification can be based
only on aggregate counter deltas.

## Conditional inline authorization

The runner MUST default to no inline cell. It may create a separate
authorization record only when:

1. the worker cell observed zero measurement-window timeouts; or
2. the analyzer identifies a specific ambiguity that an inline treatment can
   discriminate.

Authorization does not automatically start the cell. No rerun or replacement
is permitted.

## Required artifacts

```text
build-manifest.json
runtime-profile-manifest.json
commands.json
topology.conf
peer-a-summary.json
peer-b-summary.json
peer-a.trace.log
peer-b.trace.log
peer-a-delivery-latency.csv
peer-b-delivery-latency.csv
causal-timelines.jsonl
classification-summary.json
resource-summary.json
terminal.json
report.md
```
