# Tiger Qwen3 runtime qualification (2026-08-14)

## Failure in the previously accepted SIF

The exact previous Spec170 SIF was tested against the pinned Qwen3 model before
starting MiniNDN:

- SIF SHA-256: `sha256:525c4b890c4012d3f36653d0209f7decec508635818e5b0829250ef06d012af1`.
- SIF Python runtime: `transformers 4.48.2`.
- Qwen3 revision: `e6de91484c29aa9480d55605af694f39b081c455`.
- Model config declares `transformers_version: 4.51.0` and `model_type: qwen3`.
- Standalone Slurm job `189267` reached model loading and failed closed with
  `ValueError: ... model type qwen3 ... Transformers does not recognize this
  architecture`.

Therefore the old SIF's static/CUDA PASS was not sufficient for Qwen3 Gate B;
it was a real runtime compatibility failure, not a MiniNDN or GPU failure.

## Qwen3 source staging

Slurm job `189265` on `itiger05` downloaded the public model revision into
node-local `/tmp`, validated all nine files against the pinned manifest, and
atomically promoted the result to:

```text
/project/tma1/ndnsf-di/models/source/qwen3-0.6b/e6de91484c29aa9480d55605af694f39b081c455/
modelDigest=sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db
totalBytes=1519197900
```

The earlier attempts are retained: `189263` correctly rejected a non-writable
`/scratch`; `189264` downloaded successfully but failed in its manifest sum
expression before promotion. Neither left a partial project target.

## Remediation

Commit `0a90d332e34a6a888a017b54a34f79e5fad7b500` on `Experimental`:

- locks `transformers` to `4.51.0`;
- requires `transformers.models.qwen3.configuration_qwen3` in the image static
  probe;
- adds lock/probe regression tests.

The repaired immutable GPU workflow is `31758215135`; its resulting OCI digest
and SIF must be recorded separately. The old SIF remains diagnostic only and
must not be used for Gate B or freeze evidence.
