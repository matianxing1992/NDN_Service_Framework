## Material Passport

- Origin: local filesystem, Docker, and swap inspection
- Origin Date: 2026-07-31
- Verification Status: VERIFIED

## Scope

This audit determined why the local root filesystem reached 97--100% usage
during Spec 165/166 validation and removed only rebuildable, non-canonical
data. It did not alter source code, recorded verdicts, logs, the retained
canonical model artifacts, or the active candidate image.

## Finding

The dominant avoidable consumer was repeated per-run Qwen export/split output,
not NDNSF-DistributedRepo storage. Each fresh Gate B preparation copied roughly
5--6 GB into its run-local `qwen-onnx-stage-artifacts` directory. The inspected
DistributedRepo stores and campaign state were only kilobytes to tens of
megabytes and therefore cannot explain the exhausted filesystem.

Other material consumers were the retained canonical artifacts, the local
build tree, shared Docker image layers, editor indexing cache, Hugging Face
model cache, and Codex session history.

## Removed Data

The following exact, rebuildable paths were deleted:

- `results/spec165-local-gates/20260731T045507Z-3aeb57ea/minindn/runtime/qwen-onnx-stage-artifacts` (about 6.0 GB; superseded artifact copy)
- `results/spec165-local-gates/20260731T074943Z-346da1f8/minindn/runtime/qwen-onnx-stage-artifacts` (about 5.0 GB; failed/truncated artifact copy)
- `results/spec165-local-gates/20260731T075231Z-753be3db/minindn/runtime/qwen-onnx-stage-artifacts` (about 5.2 GB; failed/truncated artifact copy, removed during failure triage)
- `/home/tianxing/.cache/vscode-cpptools/ipch` (about 4.1 GB; rebuildable editor index)

The failed-run verdicts, summaries, logs, workloads, and lifecycle evidence
were retained. Docker dangling image records were pruned, and 336.2 MB of
Docker builder cache was released. The dangling records shared their large
layers with the retained candidate image, so deleting those records did not
release another nominal 9 GB.

These deletions are not recoverable directly; every deleted item was either a
cache or reproducible from the retained model source and scripts.

## Retained Data and Safety Boundary

- Canonical prepared run:
  `results/spec165-local-gates/20260731T074249Z-1edd6ad0`
- Canonical stage hashes:
  - stage 0: `6f02058ba2cc420b4f11c6e7ab391451c3fbdb9bc4ca4ba8adebd633de60ec06`
  - stage 1: `b9e4acb15b5ea2ba009f4bd6a132ce7efb90c18b5062aadc6769bd5514f3e501`
  - stage 2: `b2cb724a2f4638fee6c008c0f657ce687a1de262b67b8cbaf351aca6033ac841`
- Candidate image: `ndnsf-di:spec165-minindn-gate`
- Qwen3-0.6B source cache under `/home/tianxing/.cache/ndnsf-spec163-hf`
- Codex session and recovery history under `/home/tianxing/.codex`
- `/var/tmp/ndnsf-python-binding-build.swap`: this 4 GB file is active swap,
  with about 0.5 GB in use at audit time, and is not garbage.

## Before and After

- Before: `/dev/sda5` had about 5.2 GB available and was 97% used.
- After: `/dev/sda5` had about 20 GB available and was 89% used.

## Prevention Contract

Future local Gate B/C runs must use:

```text
--reuse-prepared-run results/spec165-local-gates/20260731T074249Z-1edd6ad0
```

Longer term, prepared model artifacts should live in one content-addressed
store. Per-run directories should contain manifests and links, not independent
multi-gigabyte copies. Failed runs may retain evidence, but their reproducible
artifact inputs should be removed after hashes and failure facts are recorded.
