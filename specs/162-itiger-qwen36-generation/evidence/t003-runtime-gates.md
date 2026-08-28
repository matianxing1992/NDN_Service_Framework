# T003 Qwen3.6 Runtime Gates

Date: 2026-07-28

## Result

`PASS`

This result closes runtime compatibility, OCI/SIF materialization, CUDA
visibility, and the collaboration operation-status binding. It is not model
capacity, model loading, generation correctness, or distributed-inference
evidence.

## Immutable identities

| Artifact | Identity |
|---|---|
| Qwen3.6 wheel lock | `sha256:aedbff59b78a23f2288b8228d18a9d45dee7194cf13e5fa6d19b02311c2dc5c4` |
| Local image | `sha256:59c71dedf8736daf2e363b4b10b8b113c63c5d50ee0fe83619b283975eaf87c7` |
| OCI manifest | `sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29` |
| Streamed archive | `sha256:bfac393e381f43ff954e49a70d48adc4a05f11fdf2584f58498f6f1f4f51254b` |
| SIF | `sha256:6bb55d85bd1244e30d0233d84c753be887e527695422ba47df9ec89c20c1d072` |

The parent layered build records `developmentCandidate=true` and app source
seal
`sha256:bcc175cd8c99ef250150379f127645327216816844d97eb0e30fc92edb0dccc1`.
The immutable identities above therefore describe a frozen development
candidate, not a clean Git or production release.

The 19-wheel closure includes Transformers 5.14.1, tokenizers 0.22.2,
huggingface-hub 1.25.1, safetensors 0.8.0, and regex 2026.7.19. The retained
runtime probe imports the public Qwen3.5 text config, decoder layer, RMS norm,
rotary embedding, causal-mask, and recurrent-attention-mask APIs.

## Gate results

| Gate | Result | Evidence |
|---|---|---|
| Overlay tests | 4/4 PASS | `tests/container/layered/test_qwen36_overlay.py` |
| Read-only local runtime probe | PASS, CUDA unavailable as expected | local image probe |
| Local Docker collaboration | PASS, state 1/2/3/4 | `results/spec162-itiger-qwen36-generation/local-docker-operation-status-20260728T162T003B/` |
| SIF materialization | Job 174578, `COMPLETED 0:0`, itiger04, 12:47 | `results/spec162-itiger-qwen36-generation/materialization-6216ef2b9525-001/` |
| RTX SIF collaboration | Job 174594, `COMPLETED 0:0`, itiger07, 00:53 | `results/spec162-itiger-qwen36-generation/operation-status-sif-smoke-6216ef2b9525-001/` |

The RTX job used one `NVIDIA RTX 5000 Ada Generation` with 32760 MiB,
`apptainer exec --nv`, two CPUs, and 5 GiB Slurm memory. Its runtime probe
reported `cudaAvailable=true`. The fake-payload collaboration ran one NFD,
Controller, Provider, and User; completed one assignment; and recorded
selection status states 1, 2, 3, and 4. No Qwen model was loaded.

## Preserved lessons

1. The probe CLI requires `--lock`; omitting it is an operator error.
2. Read-only containers require the probe to disable pip cache access.
   Unexpected `pip check` dependency errors remain fatal.
3. Private GHCR anonymous pulls return 401 on this path. Do not make the
   package public or pass credentials through Slurm. Stream the exact compressed
   Docker archive directly to `/project`, verify its checksum, and convert it
   only in allocation scratch.
4. Do not create the large archive on the local filesystem. The completed
   archive occupies 4.4 GiB on Tiger while the promoted SIF occupies 4.1 GiB.
5. Verify relative checksum manifests from their containing evidence
   directory.

## Remaining boundary

No model weights were downloaded or loaded. T004 must prove actual reference
and stage capacity with strict CUDA behavior before T005 may receive separate
authorization for one preparation allocation and one three-node generation
smoke. Job 174382 belongs to Spec 161 and was not changed.
