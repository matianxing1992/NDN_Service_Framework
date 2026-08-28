# T008 Candidate Container Evidence

Status: PASS.

Gate C used candidate image ID
`sha256:dcef2858c060ba0ba01903dd57ca5d3e844d1c1f9b500ed59f5dba9dd753ac47`
with real MiniNDN inside a privileged, memory-bounded container. The candidate
used its sealed ABI-matched NDNSF and DistributedRepo native extensions while
current application/experiment code and the same immutable model artifacts
were mounted read-only.

The closure container record used the same workload/model digests as Gate B,
reported no OOM, and passed eight rows, six measured rows, and 64 distributed
token requests with eight tokens per invocation. Missing image, model,
backend, or mandatory evidence remains a blocking failure.
