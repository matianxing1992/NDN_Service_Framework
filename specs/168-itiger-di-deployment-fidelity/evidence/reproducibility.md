# Reproducibility status

The accepted small-model control and repeated schedule are immutable:

- Job 182518 is the one-request control (`v88`), with one wire Request, three
  CUDA stages, one authenticated Response, and zero CPU fallback.
- Job 182777 is the complete 30-row same-allocation schedule. Reanalysis of the
  untouched remote evidence is `PASS` with 1 cold row, 29 warm rows, 30 wire
  Requests, zero token Requests, and zero CPU fallback. Its derived analysis
  digest is `sha256:14b587e55f9546556bac47ed2c165523ca230880ba106538b316070433ab3e64`.

The clean-allocation comparison required by T016 was **not submitted**. The
large-model prerequisite did not pass: v93 Job 182780 reached verified
Stage-0 transfer and then hit the TigerCluster memory cgroup limit; the sole
FR-019 resource-repaired v94 Job 182782 fetched all three shards, reached three
CUDA `RUNTIME_READY` markers, and returned one authenticated response, but the
exact deterministic reference check failed (`TOKEN_MISMATCH`). Submitting
another large model run merely to populate a reproduction table would violate
the campaign's single-attempt rule.

Consequently there is no claim of cross-allocation reproducibility for the
large model. The missing comparison is an explicit residual gap, not an
imputed success.
