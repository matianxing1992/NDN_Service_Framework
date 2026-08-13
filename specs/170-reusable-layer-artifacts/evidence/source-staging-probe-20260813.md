# Spec170 source staging probe (2026-08-13)

The current `Experimental` source candidate is `fb49fd98db7210b17233ca39c3fc9c6ed82cc869`.
To avoid unrelated release/UAV payloads, the staged archive excludes `docs/`,
`RELEASE/`, `NDNSF-UAV-APP/`, build/cache trees, PDFs/PPTX files, and the two
tracked standalone sync demo binaries. The archive is source staging only;
it is not an OCI or SIF identity.

```text
archive=/project/tma1/ndnsf-di/candidates/spec170-source-fb49fd9/source.tar.gz
sha256=9ec4dd7865c76b1a54f24ce7f1af578ffde2dda936618f70a67c6649c726dfd2
```

The archive and the immutable probe script were transferred with a partial
file followed by SHA-256 verification and an atomic rename. Slurm job `188138`
ran on `itiger05` in `bigTiger` and completed in one second with exit code 0:

```text
SPEC170_SOURCE_PROBE_PASS|commit=fb49fd98db7210b17233ca39c3fc9c6ed82cc869|archive=/project/tma1/ndnsf-di/candidates/spec170-source-fb49fd9/source.tar.gz|sha256=9ec4dd7865c76b1a54f24ce7f1af578ffde2dda936618f70a67c6649c726dfd2
```

The probe checked archive readability, extraction, the compact-key-id source
fix, and the Spec170 specification file. It did not invoke Apptainer, CUDA,
Qwen, or any D0/D1/D2 gate. The earlier interrupted full-archive transfer is
preserved outside the candidate as `/tmp/spec170-source-6eb237a-interrupted.tar.gz`
on the Tiger login environment and is not used by any candidate.
