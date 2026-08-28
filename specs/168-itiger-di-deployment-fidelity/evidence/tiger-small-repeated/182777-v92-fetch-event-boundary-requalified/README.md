# Job 182777 v92 requalified analysis

Slurm job `182777` ran the frozen 30-row small-model campaign once with
source bundle `sha256:4b57ef47badddcf0df84fd2c5f660119c8d2cb81aea2106f6195c0ac2e3717ef`,
source identity
`sha256:ce906305d0e5e5bf264898ee4380aa9fa8c4c46ce768d767081e4e7a3fe54611`,
the pinned runtime SIF, and the small-model stage manifest
`sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.

The batch wrapper retained the terminal `FAILED` state because its immutable
v92 analyzer rejected concurrent stdout interleaving. The untouched remote
evidence was re-analyzed with the repaired analyzer and produced
`analysis.json` with `status=PASS`, `requestCount=30`, `coldRequestCount=1`,
`warmRequestCount=29`, `wireRequestCount=30`, `tokenRequestCount=0`, and
`cpuFallbackCount=0`. The requalified analysis SHA-256 is
`sha256:14b587e55f9546556bac47ed2c165523ca230880ba106538b316070433ab3e64`.

The repair treats fetch/ACK markers as concurrent streams: it counts the
authoritative marker occurrences and checks complete request-id coverage,
while retaining the raw failed directory and the user-side one-request/
one-ACK-closed lineage. It does not rewrite or delete the original Slurm
terminal state.

Remote raw evidence:

`/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated/spec168-campaign-v3-b6fc1862e38d04dda2aa-FAILED-job-182777`

The complete 730,720-byte analyzer result is retained in `analysis.json`;
large provider/repository logs remain at the remote evidence path to avoid
duplicating payloads locally.
