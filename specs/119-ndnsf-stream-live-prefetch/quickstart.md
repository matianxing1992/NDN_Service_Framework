# Quickstart: Validate NDNSF Stream Live Prefetch

## Prerequisites

- Build from the current repository with ndn-cxx/NFD/MiniNDN available.
- Complete the Spec 118 Data admission gate before the encrypted network matrix.
- Use unique result directories and preserve failures; do not tune and rerun a
  failed repetition under the same candidate identity.

## 1. Core Deterministic And Python-Parity Gate

```bash
./waf build --targets=ndn-service-framework,unit-tests -j4
cd pythonWrapper && python3 setup.py build_ext --inplace && cd ..
/lib64/ld-linux-x86-64.so.2 ./build/unit-tests --run_test=Stream
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py
```

The explicit ELF loader is a local-VM workaround for a mounted-executable
permission anomaly. On hosts where `./build/unit-tests` executes normally, use
the binary directly.

Required vectors:

- fixed-capacity signed-map codec/resolver parity,
  `block=floor(cursor/B)` lookup, typed names, one-Data Manifest/wire cap,
  exact original-name round-trip, reverse lookup, predeclared tombstones,
  terminal-unproduced predictions, version reset, continuity, and cache bounds;
- overlap, remap, same-name equivocation, cross-session reuse/stale cache, fork,
  gap, malformed/unbounded/unroutable name, wrong Provider/version, and late
  Mapping rejection/classification;
- cached burst then stable live samples;
- multi-packet publication-group bursts;
- literal paper Eq. (3) reproduction and separately named NDNSF profiles;
- retrieval-delay/sample-period demand;
- item underestimation to later cursors and immutable overprediction;
- aggregate Mapping/payload/retransmission budget, Congestion Nack/Mark, and
  known-produced timeout versus future-generation wait;
- detection-period hysteresis;
- delay step versus timeout/Nack pressure;
- recovery before/after playout deadline;
- invalid and old-session observations.

For the ordered implementation boundary, Spec 119 T001 is only the first two
bullets above. Its C++ and Python tests both consume the frozen
`map-wire-v1.json`, `resolver-traces-v1.json`, `map-rejections-v1.json`, and
`frontier-retention-v1.json` fixtures. The remaining controller vectors belong
to T002 and are not implied by completing T001. The 2026-07-18 T001/T002
checkpoint is 33 passing C++ `Stream` cases and 18 passing Python Core-streaming
cases, including `controller-traces-v1.json` and mapped-pressure rollback.

## 2. UAV Policy And Producer Admission Gate

Before the UAV gate, run the app-neutral public API regression:

```bash
python3 tests/python/test_ndnsf_live_stream_minindn.py

sudo -n -E python3 Experiments/NDNSF_LiveStream_Minindn.py \
  --loss 0 --count 12 --consumers 2 \
  --output results/spec119-live-stream-minindn-dual-20260718-candidate2

sudo -n -E python3 Experiments/NDNSF_LiveStream_Minindn.py \
  --loss 5 --count 12 --start beginning --fec \
  --output results/spec119-live-stream-minindn-fec-loss05-20260718-candidate4
```

The dual-consumer run proves `Beginning` and a later refreshed-descriptor
`Latest` consumer against one Provider. The FEC-enabled run proves the public
`reserveGroup`/`publishGroup` wire path over opaque bytes under network loss.
Deterministic C++ tests inject every individual source loss; a network run is
not required to fabricate an FEC recovery when normal retransmission succeeds,
and a zero-recovery observation must remain reported honestly.

Together they must prove `createLiveStream`/`reserveAhead`/`publish` and
`openLiveStream`/`start`/`status`/`stop` using semantic binary Data names,
Provider validation, predictable Mapping, bounded future Interests, and no UAV
types. It must also exercise `reserveGroup`/`publishGroup` with FEC disabled and
with one XOR repair over random opaque bytes, lose each source position, verify
byte-identical recovered callback content, and reject wrong signer/group/digest/
length, corrupt repair, two-loss, oversize, and expired groups. Inspect the C++
and Python surface plus logs to prove no key/cipher/encrypt/decrypt field exists
and prove recovered bytes are not cached or republished under the missing Data
name. Spec 118 is blocked until this gate passes.

```bash
./build/unit-tests --run_test=UavProtocolState
```

Verify the validated contract version, roots, five frontiers, safe join cursor,
publication/FEC-group sample unit and measured period initialize the resolver;
signed Mapping blocks arrive before the advertised horizon; payload Data uses
the version-unique semantic UAV name; one publication group updates the detector
once; underestimated items use later cursors without prefetch credit;
overpredicted names remain immutable while their Interests become terminal;
and unmapped/ambiguous/tombstoned/stale/too-far/over-cap future Interests
allocate no producer pending state. Duplicate Interests reuse identical signed
Content. Any fallback to `/<stream-prefix>/<packetSeq>` fails the gate.

## 3. Security Ordering Gate

Run the Spec 118 focused contract first. Prove wrong mapping signer/authority,
version, overlap/remap, gap, bounds, or malformed original name produces no
payload Interest. Then prove wrong payload signer/name/session, replay, corrupt
ciphertext/tag, and stale key produce zero prefetch estimator, FEC,
retransmission, or decoder updates.

## 4. Matched MiniNDN Campaign

Use the existing UAV campaign topology and a unique campaign root. Each cell
has at least five fresh-process repetitions and a 60-second measured window.

Core cells (same payload names and workload):

```text
policy=mapped-pressure,           loss=0%,5%
policy=mapped-live-v1-future-on,  loss=0%,5%
policy=mapped-live-v1-future-off, loss=0%,5%
```

The frozen executable command is:

```bash
python3 Experiments/NDNSF_LiveStream_Prefetch_Campaign.py \
  --out results/spec119-live-prefetch-acceptance-20260718-candidate1 \
  --runs 5 \
  --duration-seconds 60 \
  --loss-percentages 0,5 \
  --order-seed 11920260718
```

The orchestrator writes `campaign-plan.json` before execution, records the
source-video SHA-256 digest, and refuses to run over a nonempty partial cell.
It reuses only a cell that already has a complete summary, so failed or
interrupted repetitions are never silently retried under the same candidate.

For each `pair_id`, freeze the source-trace digest, topology, control schedule,
and logging configuration. Use a recorded random seed to counterbalance policy
execution order so warm-cache/load drift is not always assigned to one
candidate. The installed `tc netem` exposes no seed option; record this and
treat loss realization as independent run noise.

Record per repetition:

- terminal status and scheduled control completion;
- time to first decoded sample and declared live edge;
- p50/p95 capture-to-decode live lag;
- Interest, future-wait, timeout, Nack, duplicate, FEC, retransmit, and skip
  counts;
- packet window/lookahead/lifetime and phase residence;
- five frontiers, Mapping RTT/lead ratio, late mappings, starvation duration,
  map blocks/bytes/Interest share, gaps, and rejection reasons;
- eligible future opportunities and Provider-side Interest-before-production
  hits, correlating both hosts by session/cursor rather than clock subtraction;
- Nack reasons, CongestionMarks, aggregate in-flight maximum, application
  pending maximum, and NFD PIT maximum;
- consumer/producer pending maxima and decoder occupancy;
- false live-edge and security rejection counts.

## 5. Adoption Rule

Do not select the candidate by latency alone. It must preserve correctness,
security, completion, and bounded-state gates. Only
`mapped-live-v1-future-on` applies the Mapping/future-hit gate: every run must
have nonzero Mapping-ready and eligible-future denominators, and at least 99% of
eligible, nonterminal opportunities must be resolved before scheduling and
arrive at the Provider before production. Future-off records both as N/A rather
than a vacuous pass/fail. Compare
mapped-live-v1-future-on with mapped-pressure for adoption:
it must improve p95 live lag or timeout/Nack load in at least four of five pairs,
with at least 10% median paired improvement, without worsening the other metric
by more than 5%. Compare it with the same live controller's future-off mode for
future-pending value. Report Mapping bytes and Interest share directly.
Otherwise preserve the negative result and keep
mapped-pressure as default.

Report run-level distributions and paired effect sizes. Frame-level samples are
not independent experimental repetitions and must not be used to inflate
statistical confidence.
