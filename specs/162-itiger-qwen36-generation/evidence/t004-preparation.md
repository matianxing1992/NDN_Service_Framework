# T004 Qwen3.6 Preparation Contract

Date: 2026-07-28

## Result

`IMPLEMENTATION_PASS_CAPACITY_BLOCK`

The preparation implementation and no-download RTX scratch probe pass. The
durable capacity decision is blocked, so this result does not authorize or
claim model download, model load, reference generation, stage creation, or
distributed inference.

## Frozen source and runtime

```text
sourceBundleSha256=6a4629a4640df8b82ac748fb532a1db7baad91983628cac530e7c4be1cdb4643
runtimeSifSha256=6bb55d85bd1244e30d0233d84c753be887e527695422ba47df9ec89c20c1d072
model=Qwen/Qwen3.6-27B
revision=6a9e13bd6fc8f0983b9b99948120bc37f49c13e9
dtype=bfloat16
layers=64
ranges=[0,21),[21,42),[42,64)
```

`jobs/prepare-reference.sbatch` requests one node, three RTX 5000 GPUs,
eight CPUs, 256 GiB host memory, and an eight-hour ceiling. It executes only
inside the SIF with `apptainer exec --nv`. The Python preparation path:

1. downloads the exact revision into allocation scratch;
2. hashes every source/tokenizer file;
3. loads the text-only Qwen3.5 causal LM over all three visible GPUs;
4. generates frozen greedy/EOS references for five prompts with thinking
   disabled and no KV cache;
5. writes three contiguous stage packages;
6. unloads the reference and loads each stage on one allocated GPU;
7. performs one stage forward and records peak allocated/reserved CUDA bytes;
8. requires zero CPU fallback and at least 1 GiB device headroom;
9. promotes only accepted stage/reference/tokenizer/policy artifacts;
10. removes allocation scratch and never promotes the Hugging Face cache.

Actual execution of these steps is not part of T004.

## Tests

The intended RED checkpoints were:

- missing `prepare-qwen36.py` and `capacity-preflight.py`;
- Qwen3.6 nested text-stage export returned no weights.

The GREEN result is:

```text
preparation contract tests              4/4 PASS
Qwen3.6/Qwen2 stage tests              12/12 PASS
same tests in sealed 5.14.1 image       PASS
Python compile                          PASS
shell syntax                            PASS
Slurm RTX-only resource scan            PASS
```

The two read-only-image verification command errors were environmental and
preserved: the first omitted a writable temporary HOME for NDN; the second
left Python bytecode targeted at the read-only source mount. With
`HOME=/tmp/home` and `PYTHONPYCACHEPREFIX=/tmp/pycache`, the same tests and
compile passed without relaxing the read-only root filesystem.

## No-download capacity probe

Job 174610 ran on itiger07 with three RTX 5000 Ada GPUs. It wrote and fsynced
64 MiB in allocation scratch, then removed it. The evidence records:

```text
state=PASS
scratchFreeBytes=15251694170112
scratchTemporaryPeakRequiredBytes=171798691840
downloadPerformed=false
modelComputePerformed=false
```

Local evidence:
`results/spec162-itiger-qwen36-generation/capacity-6a4629a4640d-001/`.

## Blocking durable decision

The current project tree uses 81,003,067,904 bytes. The conservative durable
projection is 158,312,479,232 bytes, or 179,787,315,712 bytes with the 20 GiB
reserve. Those values fit the global NFS free-space observation, but no
authoritative user/project quota was available:

```text
allowed=false
blockReason=DURABLE_QUOTA_UNVERIFIED
verifiedQuotaBytes=0
cleanupAuthorized=false
```

The input and exact decision are retained as
`t004-capacity-input.json` and `t004-capacity-decision.json`. T005 must not run
until a storage administrator or authoritative quota tool supplies the
controlling limit, the decision is regenerated to `allowed=true`, and the user
separately authorizes the exactly-once preparation and generation smoke.
