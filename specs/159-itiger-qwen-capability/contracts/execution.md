# Execution Contract

## Materialization

- Input is `ghcr.io/matianxing1992/ndnsf-di@sha256:<digest>`.
- Work executes in one CPU Slurm allocation.
- Output is `runtime.sif`, its SHA-256, logs, and a terminal JSON record.
- Promotion occurs only after local scratch verification.

## Standalone GPU

- One requested GPU is visible in Slurm and inside `apptainer exec --nv`.
- PyTorch reports CUDA and executes a device operation.
- The exact mounted Qwen revision produces non-empty deterministic output.
- CPU fallback or a changed model fails.

## NDNSF-DI GPU

- One job-local NFD is started and stopped by the job.
- Controller, provider, and requester use the normal dynamic secured API.
- The Qwen provider reports the selected GPU backend.
- A single request ID correlates request, ACK/selection when applicable,
  provider execution, generated tokens, and response.
- Success requires requester-visible Qwen output, not provider-only logs.
