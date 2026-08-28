# T001 Baseline and Reusable-Material Freeze

**Status**: PASS for lineage/material inventory; no implementation or campaign
admission is claimed.

## What was verified

On 2026-08-03, a fresh read-only TigerCluster check resolved the existing
runtime and model assets used by the retained controls:

- The shared 4,367,683,584-byte SIF still exists read-only at
  `/project/tma1/ndnsf-di/releases/spec167-native-file-producer-0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92/runtime.sif`.
  A fresh full `sha256sum` is
  `1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`,
  matching jobs 181948 and 181951.
- The Qwen3-0.6B stage manifest still hashes to
  `8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
  Its three 594,357,850 / 283,192,711 / 625,825,930-byte artifacts exist in
  mode `0400`, each with link count 3 and the recorded content digest.
- The Qwen3.6-27B stage manifest still hashes to
  `16de41986564c436a33cd6a292f17f4bd939d0a893c441441022e3b389737f6f`.
  Its three 18,530,194,174 / 15,987,396,002 / 19,274,718,182-byte artifacts
  exist in mode `0400`, each with link count 2 and the recorded content digest.
  This confirms metadata-only/hard-link reuse; no 53.79 GB payload copy was made.
- The five-prompt contract, current single/formal analyzers, Slurm launcher, and
  rank launcher were hashed locally. They are inputs for T002 audit and are not
  assumed to be the exact sealed sources used by an earlier job.

All machine-readable paths, sizes, inode/link observations, content identities,
job/evidence references, and claim boundaries are in
`baseline-manifest.json`.

The local integrity validator returned `BASELINE_MANIFEST_PASS` and verified all
nine local referenced hashes. The manifest digest is recomputed whenever this
claim-boundary record changes; it is not a remote-campaign success signal.

## Frozen evidence meaning

- Job 181948 is a **measured complete-inference sub-result inside a failed
  campaign**, not a full positive-control PASS. The retained User process exited
  0 after one Qwen3-0.6B Request produced a 47-token EOS response with an exact
  reference match. Its log contains exactly one
  `NDNSF_DI_AUTOPLANNING_REQUEST_SENT` and one final response; all three
  Providers entered FULL execution, completed Repo fetch, reported
  `cpuFallback=false`, and released their reservations. However, `sacct`
  classifies the job and batch step as `FAILED`, `result.json` records
  `state=FAIL`/`exitCode=1`, and the retained analyzer stopped at
  `provider 0 lacks immutable artifact readiness`. The inference result is
  therefore usable only as a narrow execution-path baseline.
- Job 181951 is a measured negative control: Qwen3.6-27B Stage 1 stopped in
  DistributedRepo segmented fetch after 630,800 bytes; it never loaded on CUDA
  and produced no complete Response. It is not a large-model inference result.
- Spec 167 job 181822 is repository-only transport evidence. Its 60-row campaign
  cannot be used to infer end-to-end model preparation or inference throughput.

### Job 181948 coverage boundary

Proven by retained raw evidence:

- three distinct RTX 5000 nodes executed the three CUDA stages;
- one durable FULL request returned the exact 47-token EOS answer;
- each Provider completed the already-published stage fetch and terminal release;
- the User process exited successfully under the recorded SIF, source, model,
  and stage-manifest identities.

Not proven by Job 181948:

- an overall Slurm/campaign/analyzer PASS;
- the current immutable-artifact readiness schema or current exact-SIF gate;
- the current request-first ACK-driven split/role plan;
- dynamic DistributedRepo publication and fetch within one durable invocation;
- removal of fixed settle waits or progress-driven deadline behavior;
- cold-start versus subsequent cache-reuse latency;
- multiple measured requests with stable requestId, Provider session, model
  identity, role assignment, dependency lineage, and final Response.

Accordingly, the baseline restoration goal is not to reproduce an alleged old
full PASS. It is to preserve this narrow successful execution slice, then add
one missing lifecycle boundary at a time and require real MiniNDN plus exact-SIF
evidence before a new TigerCluster submission.

## Workspace boundary

The current repository is intentionally dirty (170 status entries at this
checkpoint) and is not a frozen candidate. T001 does not hash the current tree
as a deployable source identity, modify user changes, or submit work. A future
candidate must be created only after the implementation/local admission tasks
pass and must record the exact source delta independently.

## No repeated work

T002 and later launchers must resolve the above content-addressed material by
path and digest. Run directories may store links, manifests, logs, and analysis
only. A missing or mismatched artifact blocks admission; it does not authorize a
foundation rebuild, model re-preparation, or silent replacement.
