# Multi-Node Execution Contract

## Gate A: allocation-local transport

- Exactly three distinct nodes and three allocated RTX 5000 GPUs.
- One job-local NFD per node.
- Explicit faces/routes are recorded.
- A named Data packet is produced on one node and fetched on another with an
  exact payload digest.
- Failure prevents the inference submission.

## Gate B: frozen stage artifacts

- Qwen revision and full-model digest match Spec 159.
- Three FP16 packages cover `[0,8)`, `[8,16)`, and `[16,24)` exactly once.
- Every artifact has immutable size and SHA-256.
- Stage loading and a CUDA operation pass inside the accepted SIF.

## Gate C: collaborative inference

- Stage 0, Stage 1, and Stage 2 run on distinct physical nodes and GPU UUIDs.
- Normal NDNSF-DI REQUEST, ACK, SELECTION, collaboration assignment, dependency
  transfer, and RESPONSE are present.
- Stage 0-to-1 and Stage 1-to-2 objects cross node boundaries and match their
  planned names and digests.
- Every provider reports CUDA execution with CPU fallback false.
- The requester-visible final top token and shape equal the frozen reference.

## Exactly-once and evidence

Each submission has a unique run ID, rendered-script digest, one Slurm job ID,
one terminal state, and a durable evidence directory. Started failures are
retained. Replacements are new linked identities and are never automatic.
