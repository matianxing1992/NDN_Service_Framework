# Quickstart Validation Guide

The Spec 177 contract, CPU adapter, functional fixture, and real MiniNDN
fixture are implemented on `UAV-Experimental`.  The commands below keep the
transport gate separate from the still claim-bounded scientific evaluation.

## 1. Fixture and contract integrity

```bash
python3 -m pytest tests/python/test_uav_multiview_contract.py -q
```

Expected: all six files match the manifest; request/result rules, duplicate views, and functional-only labeling pass.

## 2. C++ unit contract

```bash
./build/unit-tests -t 'UavMultiView*'
```

Expected: serialization, correlation, minimum-view policy, result completeness, and failure taxonomy pass.

## 3. CPU integration

```bash
python3 -m pytest tests/python/test_uav_multiview_integration.py -q
```

Expected: a deterministic adapter consumes 2-6 verified views; one-view and mixed-target cases fail explicitly.

## 4. Real model functional gate

```bash
python3 NDNSF-UAV-APP/tools/run_multiview_fixture.py \
  --manifest NDNSF-UAV-APP/testdata/multiview-car/manifest.json \
  --views 6 \
  --output results/uav-multiview-fixture
```

Expected: one fused car decision, six annotated images, accepted-view provenance, model digest, and stage timings. This is not an accuracy benchmark.

## 5. MiniNDN multi-process gate

```bash
python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py \
  --fixture NDNSF-UAV-APP/testdata/multiview-car/manifest.json
```

Expected for the unprivileged contract gate: a deterministic topology
description with at least two UAV producers, two compute Providers, one
terminal owner, and named failure scenarios.  The real-transport entry point
requires MiniNDN dependencies and root access; it refuses to run when those
preconditions are unavailable:

```bash
sudo -E python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py \
  --fixture NDNSF-UAV-APP/testdata/multiview-car/manifest.json --execute
```

The command requires root because MiniNDN creates network namespaces.  It
records per-node logs, routes, exact Data hashes, and a terminal summary:

```bash
sudo -n python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py \
  --fixture NDNSF-UAV-APP/testdata/multiview-car/manifest.json \
  --execute --all-scenarios --provider /provider/gpu \
  --output results/uav-multiview-minindn
```

The five scenarios are nominal, Provider-selection, unavailable-view,
late-view, and publication-failure.  Nominal and selection must complete with
one terminal Provider; the three failure scenarios must reach explicit
rejected terminal stages.

## 6. Quantitative evaluation gate

Run only after the MiniNDN gate and a dataset registration are frozen. Report
paired one-view versus 2/4/6-view metrics, completion rate, latency,
transferred bytes, failure stages, and uncertainty. Generated fixture images
are excluded from the accuracy table; the current one-sample registration is a
functional point-estimate gate with `insufficient-samples` uncertainty.
