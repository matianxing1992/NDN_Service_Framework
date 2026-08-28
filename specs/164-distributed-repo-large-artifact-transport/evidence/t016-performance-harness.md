# T016 Performance Harness Evidence

## Scope

T016 implements the measurement contract; it does not make a throughput claim.
The harness now supplies:

- four directional `iperf2` measurements across the two links in the frozen
  `publisher -> repo -> consumer` MiniNDN topology;
- a raw segmented-NDN subject using the same topology, 4096-byte payload
  geometry, adaptive fetcher, congestion window, retry policy, and logs as the
  repository subject;
- the eight canonical repository phase names, transfer counters, process CPU,
  peak RSS, and sampled `/proc` CPU/RSS/I/O observations;
- stable SHA-256 operation sampling controlled by
  `NDNSF_TIMELINE_TRACE_SAMPLE_RATE`;
- a process-level single-writer lock, canonical JSON manifest, detached SHA-256
  seal, deterministic randomized matched-block schedule, and append-only
  JSONL/CSV run ledger;
- JSON Schemas for campaign manifests, per-run records, and phase/resource
  samples.

The formal matrix contains 24 matched factor combinations:

```text
sizes       = 1 MiB, 64 MiB, 1 GiB, 16 GiB
replicas    = 1, 3
concurrency = 1, 4, 16
subjects    = raw segmented NDN, legacy exact packet,
              digest only, signed manifest
schedule    = one warmup plus five measured repetitions
```

Each repository cell names its paired raw-NDN cell. The schedule randomizes
pair blocks and subject order using the seed recorded in the sealed manifest.
Failed and inadmissible runs are append-only records; they are not overwritten
or omitted.

## Verification

Executed from the repository root:

```bash
python3 -m py_compile \
  Experiments/spec164_artifact_campaign.py \
  Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py
python3 tests/python/test_spec164_performance_harness.py
sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --performance-subject physical-network --quick-smoke \
  --measurement-window-seconds 1 \
  --output-dir /tmp/spec164-t016-physical-smoke
sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --performance-subject raw-segmented-ndn --quick-smoke \
  --payload-size 8192 --timeout-seconds 8 \
  --measurement-window-seconds 1 \
  --output-dir /tmp/spec164-t016-raw-smoke
sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --quick-smoke --payload-size 8192 --timeout-seconds 8 \
  --output-dir /tmp/spec164-t016-functional-smoke
```

Observed:

```text
performance harness unit tests: 11/11 PASS
physical-network quick smoke: PASS, four directional link measurements
raw segmented-NDN quick smoke: PASS, 8192 logical bytes, four resource samples
signed-manifest functional smoke: PASS, all eight phase keys present
performanceClaim: false for every quick smoke
```

## T017 follow-up

The T016 audit identified that the original native `SegmentedObjectProducer`
materialized the complete payload and all signed packets. T017 resolved this
before formal outcomes by adding `FileSegmentedObjectProducer`, which reads and
signs one requested segment at a time. The frozen preflight still rejects cells
whose total replica/concurrency disk, process memory, predicted completion time,
or legacy metadata rows exceed the recorded host budgets; it does not infer
large-cell admissibility merely from the bounded producer.
