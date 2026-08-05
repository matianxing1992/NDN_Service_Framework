# Spec 170 content-addressed MiniNDN diagnostic

This is a pre-Gate-B diagnostic for the reusable-artifact implementation. It
does not close T026: the current runner still exercises the established
Qwen-transformers provider path rather than the complete normal-default V3
Provider-assembly path.

## Candidate and command

- Model: `Qwen/Qwen2.5-0.5B-Instruct`, revision `main`, offline cache.
- Topology: real MiniNDN, three Provider processes, CPU transformers backend.
- Workload: one warmup request and three measured requests, two new tokens.
- Run directory: `/tmp/spec170-minindn-qwen-multi-20260804`.
- Content store: `results/.ndnsf-di-content-addressed`.
- Gate marker: `LLM_PIPELINE_MININDN_OK`.

The measured distributed latency was 406.38/416.57/418.72 ms (p50
416.57 ms). All four responses had the same model output binding (`topToken`
451), three stages, logits shape `[1, 5, 151936]`, and no CPU-fallback error
inside the transformer stage runtime.

## Storage invariant

The run directory is 244 KiB and contains three relative symlinks under
`qwen-transformers-stage-artifacts`; it contains no regular stage-weight file.
The content store contains exactly three immutable objects (477,242,930;
1,021,781,942; and 1,021,786,042 bytes) and an `index-v1.json` semantic
identity index. The object names are the SHA-256 digests and the policy keeps
the run-local paths stable for the provider loader.

The focused test
`tests/python/test_spec170_content_addressed_reuse.py` independently creates
two runs and verifies that both resolve to the same three objects, that the
run directories contain zero regular artifact bytes, and that the index is
idempotent.

## Interpretation

This establishes the disk-retention invariant and demonstrates a warm request
latency reduction relative to the first request in the same process. It does
not yet prove Provider-local canonical ONNX assembly, V3 Selection, GPU
residency, Repo retrieval, or the required three cold/warm statistical blocks;
those remain blocking work before TigerCluster qualification.
