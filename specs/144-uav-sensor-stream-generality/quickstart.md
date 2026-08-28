# Quickstart: Planned Validation Workflow

This guide is a contract for the future implementation phase. The commands
below MUST NOT be run until their referenced files exist, deterministic gates
pass, and the pre-implementation audit returns PASS.

## 1. Confirm the active feature, promoted reference, and immutable baselines

```bash
cat .specify/feature.json
rg --files specs/127-cross-application-stream-generality |
  sort | xargs sha256sum | sha256sum
rg --files specs/128-generic-multiloss-recovery |
  sort | xargs sha256sum | sha256sum
sha256sum \
  results/spec145-uav-video-runtime-20260724T064253Z/campaign-summary.json
rg -n '^\\*\\*Verdict\\*\\*: PASS' \
  specs/145-uav-video-runtime-corrections/evidence/post-implementation-audit.md
```

The active feature must be `144-uav-sensor-stream-generality`. Expected Spec
127/128 roots are recorded in [spec.md](spec.md). The Spec 145 campaign hash
must be
`8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566`.
Do not execute any Spec 127/128/145 runner.

Before implementation, record a reference-boundary table showing that both new
APPs reuse only lifecycle ownership, exact-name/security admission, callback
containment, and actual Core status. Any dependency on video class, key/delta,
FPS/GOP, codec, payload, or video threshold semantics is a blocking failure.

## 2. Run the future deterministic gates

```bash
./waf build -j2
(cd pythonWrapper && python3 setup.py build_ext --inplace --force -j2)
./build/unit-tests --log_level=message
PYTHONPATH=pythonWrapper python3 \
  tests/python/test_ndnsf_uav_sensor_stream_generality.py
PYTHONPATH=pythonWrapper python3 \
  tests/python/test_spec144_uav_sensor_stream_runner.py
python3 tests/run_uav_stream_security_contract.py
```

Every command must pass. Record exact counts and hashes rather than copying
historical counts.

## 3. Run separate non-formal MiniNDN preflights

```bash
sudo -E env PYTHONPATH=pythonWrapper python3 \
  Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py \
  --workload telemetry \
  --profile zero-loss \
  --output results/spec144-diagnostic-telemetry-preflight

sudo -E env PYTHONPATH=pythonWrapper python3 \
  Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py \
  --workload acoustic \
  --profile zero-loss \
  --output results/spec144-diagnostic-acoustic-preflight
```

Preflight verifies process ownership, qdisc discovery, readiness, sample/block
counts, metric conservation, and cleanup. These runs never count as formal
cells.

## 4. Freeze and execute the future formal matrix

Use one fresh destination:

```bash
sudo -E env PYTHONPATH=pythonWrapper python3 \
  Experiments/run_spec144_uav_sensor_stream_matrix.py \
  --output-root results/spec144-uav-sensor-stream-<fresh-id>
```

The runner must declare `formalFrozen=true`, enumerate exactly 32 unique cells,
and execute every cell once. Do not launch another MiniNDN campaign while it is
active.

## 5. Analyze without changing raw evidence

```bash
python3 Experiments/analyze_spec144_uav_sensor_stream.py \
  --input results/spec144-uav-sensor-stream-<fresh-id> \
  --spec specs/144-uav-sensor-stream-generality
```

Required outputs and formulas are defined in
[evidence-contract.md](contracts/evidence-contract.md).

## 6. Close

Recompute Spec 127/128 roots and the Spec 145 campaign-summary hash, run the
neutrality and reference-boundary audits, verify the 32-row manifest, and issue
independent telemetry/acoustic verdicts. Preserve a negative result if either
workload fails. Do not change thresholds or replace cells.
