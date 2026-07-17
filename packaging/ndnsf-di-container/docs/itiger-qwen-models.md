# iTiger Qwen model operations

Use repository commands as the canonical interface. Personal helper wrappers
never own model or evidence state.

## Storage policy

- `/project/$USER/ndnsf-di/models`: sealed source and exported model artifacts.
- `/project/$USER/ndnsf-di/cache`: bounded reusable download/build cache.
- `/project/$USER/ndnsf-di/manifests`: immutable revision, file, tokenizer,
  license, OCI, and SIF registries.
- `/project/$USER/ndnsf-di/evidence`: durable promoted evidence.
- `/tmp/$USER/ndnsf-di/$SLURM_JOB_ID`: disposable allocation scratch.
- `/home` and the workstation: no model/SIF/ONNX/cache bulk bytes.

Discover current facts without submitting a job:

```bash
tools/ndnsf-di/ndnsf-di-qwen discover \
  --host itiger --output results/spec109-itiger-qwen/discovery
```

Shared `df` capacity is never a substitute for the user's quota. A 32B or 72B
transfer requires both an authoritative live quota and a sealed upstream file
manifest peak calculation with reserve.

## Transfer and seal

Render first; submission is explicit:

```bash
tools/ndnsf-di/ndnsf-di-qwen transfer \
  --model-entry model-entry.json --run-id qwen25-0.5b-transfer-v1 \
  --output job.sbatch --ledger submission-ledger.json
# Inspect job.sbatch and preflight evidence, then repeat with --submit exactly once.
```

The transfer runs on a bounded CPU Slurm allocation, pins a 40-hex immutable
revision, retains file sizes and SHA-256 digests, quarantines partial downloads,
and promotes only a complete manifest. Never use a floating branch or an LFS
pointer as a sealed model.

The accepted 0.5B source is `Qwen/Qwen2.5-0.5B-Instruct` at revision
`7ae557604adf67be50417f59c2c2f167def9a775`. The original transfer job was
146050 and the seal job 146123; neither was replaced.

## License and cleanup

Record the model-card license before transfer. Qwen2.5 3B and 72B do not share
the Apache-2.0 class used by most other sizes and require their own review.

Cleanup always begins with a content/reference-aware dry run:

```bash
tools/ndnsf-di/ndnsf-di-qwen cleanup \
  --candidates cleanup-candidates.json --protected protected.json \
  --output cleanup-plan.json --dry-run
```

Current/prior releases, active jobs, source/candidate identities, accepted
evidence, referenced models, and all evidence roots are protected. Delete only
unreferenced diagnostic or cache content after reviewing the plan.
