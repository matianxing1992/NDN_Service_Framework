# T008 Functional Artifact Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-30
- Verification Status: VERIFIED
- Version Label: code_plan_v1

## Experiment Overview

- **Title**: Trusted single-replica artifact publication and retrieval
- **Objective**: Verify that one immutable artifact crosses the segmented NDN
  data plane, becomes active only after trust/integrity/persistence/receipt
  conditions hold, and is exposed atomically at the consumer.
- **Hypothesis**: Valid bytes produce one authenticated receipt, the exact
  `RESERVED → RECEIVING → VERIFIED → COMMITTED → ACTIVE` lifecycle, and a
  digest-identical consumer file; one-bit payload corruption produces no
  receipt, no active catalog entry, and no consumer destination.
- **Type**: deterministic functional integration

## Setup

- **Language/Framework**: Python 3.8, native NDNSF/ndn-cxx bindings, MiniNDN
- **Entry Command**:

  ```text
  sudo -n env \
    PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
    LD_LIBRARY_PATH=build \
    python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
      --scenario matrix --payload-size 65536 --timeout-seconds 20 \
      --output-dir <unique-evidence-directory>
  ```

- **Working Directory**: repository root
- **Dependencies**: local NFD, MiniNDN, OpenSSL, built NDNSF and Python bindings
- **Environment**: three wired namespaces (`publisher → repo → consumer`), 1 ms
  links, 1 Gbit/s configured link rate; this test makes no performance claim

## Inputs

| Input | Path | Description |
|---|---|---|
| Topology | `Experiments/Topology/spec164-artifact-linear.conf` | Fixed three-node wired graph |
| Payload | generated in the unique run directory | Deterministic 64 KiB byte sequence |
| Trust fixture | generated in the unique run directory | RSA-signed root, page, chunk descriptor, public key |
| Corruption | experiment role logic | One-bit mutation after the trusted manifest is frozen |

## Expected Outputs

| Output | Path | Format | Success Criterion |
|---|---|---|---|
| Matrix summary | `<output>/summary.json` | JSON | overall `verdict=PASS` and `performanceClaim=false` |
| Success record | `<output>/success/summary.json` | JSON | active receipt, exact lifecycle, atomic destination |
| Corruption record | `<output>/corruption/summary.json` | JSON | `CORRUPTION_REJECTED`, `active=false`, no destination |
| Role logs | `<output>/<scenario>/*.log` | text | no unreported traceback or hidden retry |

Fixture secrets and duplicate payload/runtime-store bytes are removed after the
verdict is captured. Public trust material, result records, and logs remain.

## Monitoring Configuration

- **Timeout**: 20 seconds per scenario
- **Monitor files**: publisher-ready, repo-result, repo-ready, consumer-result
- **Experiment type override**: generic deterministic functional integration
- **Hard failure**: nonzero role exit, missing result before timeout, invalid
  receipt, digest mismatch in success, activation in corruption, or partial
  destination visibility

## Analysis Plan

- **Primary metric**: binary lifecycle/integrity verdict for each scenario
- **Success threshold**: both scenarios pass; zero integrity and atomicity failures
- **Comparison**: success and one-bit corruption share topology, original
  payload, manifest geometry, timeouts, windows, and code path; only transmitted
  payload integrity differs
- **Statistical boundary**: no stochastic performance inference, p-value,
  effect size, or throughput claim is admissible from this functional matrix
