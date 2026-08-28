# Spec 170 Qwen3 toolchain preflight (2026-08-19)

This record explains why the first real-Qwen probe did not reach NDNSF. It is
an environment qualification result, not a protocol failure or Gate B pass.

## Model artifact

The exact pinned model was fetched once into the standard content-addressed
Hugging Face cache:

```text
repository: Qwen/Qwen3-0.6B
revision:   e6de91484c29aa9480d55605af694f39b081c455
files:      9
bytes:      1519197900
contentDigest: sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a
snapshot:   /home/tianxing/.cache/huggingface/hub/models--Qwen--Qwen3-0.6B/snapshots/e6de91484c29aa9480d55605af694f39b081c455
```

The download was initially placed one directory above the standard `hub`
root; it was moved within the same filesystem (not copied or re-downloaded)
to the path above, then its manifest was recomputed.

## Probe

The one-warmup/one-measured real MiniNDN probe used static routing, CPU
transformers, offline model loading, and a repository content-addressed stage
store. It failed during stage-policy materialization before Controller,
Provider, ACK, Selection, or Response execution:

```text
ValueError: The checkpoint you are trying to load has model type `qwen3` but
Transformers does not recognize this architecture
```

The effective host child interpreter was `/usr/bin/python3` 3.8.10 with
`transformers==4.46.3`; the r23 SIF contains Python 3.10.18 with
`transformers==4.48.2`. Both are too old for this Qwen3 checkpoint. The r23
probe therefore provides no NDNSF protocol or latency result.

As a diagnostic only, a 102-MB temporary Python overlay containing
`transformers==4.51.3`, `tokenizers==0.21.1`, and
`huggingface-hub==0.30.2` was mounted read-only into the unchanged r23 SIF.
The SIF then loaded the Qwen3 config and model and generated two tokens:

```text
SIF Python: 3.10.18; Torch: 2.6.0+cu124
qwen3_import=PASS
loaded_seconds=8.649
generated_token_ids=[3555, 525]
qwen3_model_probe=PASS
log sha256=0770068b11da95c834de2e5bd3512cede6d692503b6ac90a12259773d32f9264
```

The same overlay cannot repair the host MiniNDN path: host Python 3.8.10
fails while importing Transformers 4.51.3 (`TypeError: 'ABCMeta' object is not
subscriptable`). This confirms that installing a newer package into the host
Python would be an ABI/runtime workaround, not a valid release fix.

## Required correction before Gate B

The next candidate SIF must use a sealed Qwen3-compatible Transformers
runtime (the previously retained successful Qwen3 campaign used
`transformers==5.14.1`), with its complete wheel closure and Python ABI
recorded inside the SIF. Do not install a newer Transformers package into the
host Python or run a host-built extension against the SIF. After the new SIF
passes import/ABI/`ldd` checks, regenerate stage artifacts once in the same
sealed environment and rerun the real MiniNDN Gate B sequence.

This leaves T026/T027/T029/T036 open; it also prevents the probe failure from
being misclassified as an NDNSF protocol regression.
