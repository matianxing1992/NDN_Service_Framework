# NDNSF 100 m / 10 m/s ACK-path diagnosis

Date: 2026-08-08  
Status: 35-second mitigation falsified by independent 60-second replays;
matrix expansion remains paused pending bounded ACK and Response recovery.

## Frozen condition

Both runs used the same one-AP/four-Provider condition: 100 m range, 10 m/s,
seed 61, 5 RPS for 60 seconds, 500 ms ACK timeout, 5 s global deadline,
`FirstResponding`, `block_network=true`, and admission control disabled. They
replayed the same source trace (SHA-256
`7eddc1d9137a40bb3b9823ab638e2c6dd5fed03c21ae5e135e26dfced38e9867`).

The runtime was held fixed:

- NDNSF library SHA-256:
  `468c60c59e8d1a3786022c2853c3f4da6d2412c75bf168f2dea79e52a16ff4f7`
- Experimental NDN-SVS library SHA-256:
  `588d24a587c8a3ace33b410723e0b369941df9b40ae5f4ee385448e4af2af59e`

The baseline explicitly set `NDNSF_SVS_PERIODIC_SYNC_MS=30000`. The paired
sensitivity changed only that variable to 500 ms. Startup logs confirm the
configured value in the User and all four Providers.

## Result

| Run | Success | Mean successful latency | p95 | Timeouts |
| --- | ---: | ---: | ---: | ---: |
| 30 s periodic baseline | 116/300 (38.67%) | 103.59 ms | 265.00 ms | 184 |
| 500 ms periodic sensitivity | 116/300 (38.67%) | 113.65 ms | 358.91 ms | 184 |

The successful-request latency numbers include TRACE overhead and are not
publication-performance measurements. The important result is that both runs
had exactly the same successful request indices: requests 0--115 succeeded and
requests 116--299 timed out.

## Per-request lifecycle localization

`Experiments/analyze_ndnsf_mobility_lifecycle.py` joins each actual
`REQUEST_PUBLISHED` timestamp to the last enforced epoch in that run's
`mobility_trace.csv`, and records all Provider observations for the following
path:

```text
REQUEST_PUBLISHED
  -> provider REQUEST_RECEIVED
  -> provider ACK publication API
  -> ACK SVS_PUBLISH_BEGIN
  -> user ACK pre-decrypt observation
  -> ACK_MATCHED_PENDING_CALL
  -> PROVIDER_SELECTED
```

The two runs produced identical unique-request counts:

| Stage reached by at least one Provider/User event | 30 s | 500 ms |
| --- | ---: | ---: |
| Request published | 300 | 300 |
| Provider fetched Request | 300 | 300 |
| Provider called ACK publication | 300 | 300 |
| ACK reached `SVS_PUBLISH_BEGIN` | 300 | 300 |
| User fetched an ACK publication | 116 | 116 |
| ACK matched pending call | 116 | 116 |
| Provider selected | 116 | 116 |
| Successful response | 116 | 116 |

For all 184 failed requests in both runs, a Provider fetched the Request and
handed an ACK to SVS within the deadline. In the 30 s baseline, the median
first Provider fetch was 8.25 ms and the median first ACK SVS publication was
10.26 ms; all 184 first ACK publications occurred within 500 ms. Nevertheless,
the User observed no ACK for any of these requests. The 500 ms run shows the
same boundary and stage counts.

The first timeout occurs at the B+C to C-only boundary; the first fully C-only
request is the next request. This is a correlation, not a demonstrated
B-specific cause.
Provider fetch and ACK-publish markers continue after this transition, so the
failure cannot be explained by request execution latency, missing
`FirstResponding`, or the 5 s application deadline.

## Interpretation

This fixed replay is deterministic and localizes the failure to the reverse
ACK dissemination path between Provider-side SVS publication and the User's
SVS subscription callback. It is not evidence that 10 m/s movement during a
roughly 50--100 ms successful call causes the failures.

Shortening periodic Sync did not repair the path. In the current Experimental
NDN-SVS source, every local sequence update already calls
`sendLocalPublicationSyncInterest()`, which sends a Sync Interest immediately;
`setPeriodicSyncTime()` changes the retransmission interval/distribution but
does not itself diagnose or repair mapping/publication-fetch state. Therefore
the negative sensitivity result is consistent with a stuck state-vector,
mapping, validation, or publication-fetch path rather than insufficient
periodic frequency alone.

## Evidence and next gate

Local run roots:

- `results/ndnsf-periodic-diagnostic-20260808/baseline-periodic-30000/`
- `results/ndnsf-periodic-diagnostic-20260808/sensitivity-periodic-500/`

Each contains `request-lifecycle-coverage.csv` and
`request-lifecycle-coverage-summary.json` in addition to the raw logs and
runtime command receipt.

Do not resume the 50/75/100 m by 2/5/10 m/s matrix yet. The bounded follow-up
below closed the immediate localization question and established a safe NDNSF
default. Run one independent-process repeat of the corrected default before
using it for a larger comparison matrix.

## Follow-up: 35-second parallel-production isolation

The follow-up used a shortened 35-second window over the same seed-61 source
trace so that it still crossed the request-115/116 transition. It added only
low-overhead aggregate counters at User shutdown. A first manual replay loaded
`/usr/local/lib/libndn-svs.so`, whose ABI did not match the Experimental
headers; the invalid `version=0`, garbage timer values, and kernel segfault are
retained as rejected startup evidence. All results below instead resolve
NDN-SVS from `/tmp/ndnsf-svs-experimental-20260808/lib` with SHA-256
`588d24a587c8a3ace33b410723e0b369941df9b40ae5f4ee385448e4af2af59e`
and Boost 1.71.

| Sync production setting | Success | Mean successful latency | p95 | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Parallel, worker extra block enabled | 116/175 (66.29%) | 113.57 ms | 342.52 ms | Failure reproduced without dense NDN-SVS TRACE |
| Parallel, worker extra block disabled | 150/175 (85.71%) | 560.04 ms | 3789.01 ms | Partial improvement; parallel production remains incorrect |
| Serial, explicit diagnostic override | 175/175 (100%) | 102.61 ms | 140.39 ms | Single-variable recovery |
| Serial, new default with no override | 175/175 (100%) | 103.13 ms | 143.41 ms | Default-behavior acceptance |

The low-overhead failing replay recorded 709 User Sync processing jobs: 663
completed, 46 were recomputed through the stale-job path, and none were
dropped. All 292 User Sync production jobs completed, and the V3 rejection
counters were zero. Mapping fetch counters were also zero; mappings arrived
through Sync extensions. This rules out signature rejection, vector decode
failure, mapping timeout, and production-queue overflow as the primary cause.

A correctly linked `SVSPubSub`-only TRACE replay retained the failure (120/175)
and exposed the state propagation gap. Provider C returned to the enforced
coverage set at Unix time `1786224872.120045` and moved from about 99.5 m to
41.3 m from the AP, but the User queued no new C publication fetches for 8.786
seconds. At `1786224886.219096`, Provider A re-entered coverage; 8 ms later the
User learned C sequence numbers 69--112 in one batch and fetched the delayed
ACK publications. Thus C had created the ACKs, but the advancing C state did
not reach the User. The later PCAP/FIB diagnosis below localizes this gap to
stale NFD UDP faces and missing remote group nexthops after reconnect, rather
than to SVS state-vector merge logic.

Disabling only `NDNSF_SVS_PARALLEL_PRODUCTION` restored every lifecycle stage:
all 175 Requests were fetched, all 175 ACKs were published and fetched, all 175
ACKs matched, all 175 Providers were selected, and all 175 Responses succeeded.
The framework now treats parallel Sync production as an experimental explicit
opt-in and logs the disabled default in both User and Provider runtimes. The
successful replay is retained as timing-sensitivity evidence, not as the
causal repair; serial production alone did not pass later full-window replays.
The corrected diagnostic framework library SHA-256 is
`8eadd9f6e7da09dc659f50c7f828abfdb1eeca0a2a43c83b223161127a548ead`.

This 35-second result is a valid bounded observation, but the independent
60-second gate below shows that it was not sufficient to establish a complete
root cause or a safe matrix default.

Additional local run roots:

- `results/ndnsf-periodic-diagnostic-20260808/counters-boundary-35s-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/svspubsub-trace-boundary-35s-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/parallel-no-worker-extra-boundary-35s-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/serial-production-boundary-35s-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/default-serial-production-boundary-35s-v3-linked/`

## Independent 60-second falsification

Independent processes replayed the same canonical source trace (SHA-256
`7eddc1d9137a40bb3b9823ab638e2c6dd5fed03c21ae5e135e26dfced38e9867`)
with the correctly linked Experimental NDN-SVS library. The trace had at least
one reachable Provider during 99.5% of the 4--64 second measurement epochs, so
the large failure fractions are not explained by simple lack of coverage.

| Configuration | Success | Mean successful latency | p95 | Diagnostic status |
| --- | ---: | ---: | ---: | --- |
| Serial production, parallel receive, 30 s periodic | 132/300 (44.00%) | 150.79 ms | 391.44 ms | canonical replay; failed |
| Serial production, parallel receive, 500 ms periodic | 116/300 (38.67%) | 114.33 ms | 347.86 ms | periodic change applied; failed |
| Serial production and serial receive | 282/300 (94.00%) | 109.99 ms | 348.64 ms | major improvement, not closure |
| Serial production/receive plus core TRACE | 300/300 (100.00%) | 129.99 ms | 426.48 ms | diagnostic Heisenbug evidence only |
| New no-override serial defaults | 192/300 (64.00%) | 114.19 ms | 323.58 ms | independent acceptance failed |

The 500 ms periodic setting was confirmed after construction by
`NDNSF_SVS_PERIODIC_SYNC_MS ... value=500` in the User and all four Provider
logs. It did not repair the failure. Dense `ndn_svs.SyncTimeline=TRACE` changed
the outcome to 300/300, demonstrating material timing sensitivity; that run is
not accepted as a performance or correctness result.

For the no-override 192/300 run, the 108 failures separate into three concrete
classes:

- 47 Requests never produced a User-side ACK fetch;
- 19 fetched an ACK only after the 500 ms matching window;
- 42 matched an ACK and selected a Provider, but timed out waiting for the
  Response.

Provider-side traces show that Requests were fetched, ACKs were constructed,
and the providers entered the SVS publish call within the ACK window even for
the first two classes. The serial path does not currently emit a matching
`SVS_PUBLISH_DONE` marker, so these traces do not prove publication completion.
The third class is the expected limitation of the current `FirstResponding` path: it
does not retry or reselect after the chosen Provider disappears. Disabling
parallel receive removed stale worker jobs and improved one run substantially,
so both receive and production parallelism are now explicit opt-ins, but the
variation across independent processes proves that serialization alone is not
the root fix.

### Publication-completion and sequence diagnostic

After adding non-behavioral synchronous publication-completion and sequence
markers, another independent replay completed 232/300 requests (77.33%). All
300 Requests had a confirmed `SVS_PUBLISH_DONE`; successful-response mean and
p95 latency were 429.04 ms and 3138.64 ms, so the higher completion count does
not close the latency or reliability defect.

The Provider-C-only interval exposes a deterministic sequence pattern. Provider
C completed ACK publications for sequence numbers 70--112 within 9--22 ms of
their Requests, but the User fetched almost the entire range together near the
end of the interval: fetch delay fell from 8465 ms at sequence 70 to 51 ms at
sequence 112. This rules out ACK construction/encryption time and indicates
that the newest committed SVS state was not delivered during continuous local
publication. Inspection of `sendLocalPublicationSyncInterest()` shows that each
publication replaces the scoped periodic retransmission event; at 5 RPS a
shorter periodic timer can therefore be postponed repeatedly. T016 now tests
that timer mechanism alone before any Response retry/reselection change.

Result directory:
`results/ndnsf-periodic-diagnostic-20260808/publish-done-seq-full60s-replay-v3-linked/`.

The timer-starvation hypothesis was then tested in isolation. An isolated
Boost-1.71 NDN-SVS build preserved an already scheduled periodic retransmission
instead of replacing it on each local publication, and all 76 NDN-SVS unit
tests passed. With a 500 ms periodic interval, however, the fixed trace
completed only 116/300 requests (38.67%). For 184 timed-out Requests, Provider
fetch occurred within 206 ms and 183 synchronous ACK publications completed
within 409 ms, but none produced a User ACK fetch. The change is therefore not
an ACK-delivery repair and was removed from the active NDN-SVS worktree. The
next diagnostic boundary is remote state-vector receipt followed by mapping and
publication fetch.

Rejected-treatment result directory:
`results/ndnsf-periodic-diagnostic-20260808/nonstarving-periodic500-full60s-replay-v3-linked/`.

Additional full-window roots:

- `results/ndnsf-periodic-diagnostic-20260808/default-serial-production-full60s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/default-serial-production-full60s-replay-periodic500-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/serial-receive-production-full60s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/serial-receive-core-trace-full60s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/default-serial-both-full60s-replay-v3-linked/`

## NFD reconnect boundary and harness repair

A paired 35-second replay with packet capture reproduced the failure without
changing the mobility trace. It completed 148/175 requests (84.57%). During the
failed only-C interval, Provider C continued to receive five User-originated
UDP/NDN packets per second and synchronously completed successful ACK
publications, but its capture recorded zero C-to-User UDP packets. The failure
therefore preceded User-side SVS state-vector processing: the Provider's NFD
had retained unusable outgoing faces/routes after the iptables outage.

The first reconnect treatment destroyed and recreated C's remote UDP faces but
still completed only 116/175. Its NFD snapshot showed why: the new User face was
ID 302, while `/example/hello/group` retained only the local application face
ID 299; no remote group nexthop had been rebound. The corrected harness parses
the numeric face ID returned by `nfdc face create` and explicitly reinstalls the
group and identity routes on that ID whenever an NDN Provider transitions from
blocked to reachable. The same repair is enabled for NDNSF and NSC; gRPC does
not use NFD.

With numeric face-ID rebinding, a 22-second boundary replay completed 110/110,
and packet capture showed C-to-User traffic recover to 16--23 packets per
second after C reentered coverage. The current default 35-second replay then
completed 175/175 (100%), with mean successful-response latency 117.01 ms,
p95 351.65 ms, p99 528.86 ms, and 73 executions at Provider C. Admission
control remained disabled; FirstResponding, the 500 ms ACK window, the 5 s
global deadline, and the trace were unchanged.

Evidence roots:

- `results/ndnsf-periodic-diagnostic-20260808/pcap-baseline-run2-35s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/reconnect-face-state-22s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/reconnect-face-id-route-22s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/default-reconnect-repair-35s-replay-v3-linked/`

The repair then passed the SC-010 full-window gate. All three independent
processes replayed the same 60-second canonical trace with FirstResponding,
500 ms ACK timeout, 5 s global deadline, 5 RPS, disabled admission control,
and reconnect face repair recorded in the summary:

| Run | Successful | Mean latency | Reachable at publish | ACK fetched | Selected | Reachable success | Zero-coverage timeouts |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 297/300 (99.0%) | 96.03 ms | 297 | 297 | 297 | 297 | 3 |
| 2 | 297/300 (99.0%) | 95.05 ms | 297 | 297 | 297 | 297 | 3 |
| 3 | 297/300 (99.0%) | 97.93 ms | 297 | 297 | 297 | 297 | 3 |

Evidence roots:

- `results/ndnsf-periodic-diagnostic-20260808/sc010-reconnect-repair-run1-60s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/sc010-reconnect-repair-run2-60s-replay-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/sc010-reconnect-repair-run3-traced-60s-replay-v3-linked/`

The three failures per run are not ACK-path failures: the replay trace records
`A=0|B=0|C=0|D=0` at publication for all nine. Every request with at least one
reachable Provider reached ACK fetch, provider selection, and a successful
Response. Results produced with `block_network=true` before reconnect face/FIB
repair remain diagnostic only and must not be used in the paper comparison.

The current rebuilt framework library SHA-256 is
`16a811cf19d3fdcc5c5d2058c2eb55829bd5ab7f0afbcc960f200a1960f9cbc8`.
SC-010 and T016 are closed. SC-011 and T017 remain open. This establishes
correct ACK-path recovery for the fixed 100 m trace, but it is not yet a fair
cross-system mobility-advantage result for the paper.

## Validation

- Targeted Waf build of `ndn-service-framework`, `App_ServiceController`,
  `App_WifiMobilityProvider`, and `App_WifiMobilityUser`: passed.
- Full C++ unit-test binary: one unrelated wall-clock timing assertion was
  0.772 ms over its 100 ms threshold; its focused rerun passed. The prior
  correctly linked 456-test run had no errors.
- Spec171 Python suites: 34 tests passed (four-Provider profiles/repetitions,
  lifecycle alignment, comparison/figure analysis, seed repeats, explicit
  opt-in contract, and mobility harness receipts).
- Spec Kit strict audit: PASS; 18/18 functional requirements traced, 9 success
  criteria, and 14/14 tasks complete.
