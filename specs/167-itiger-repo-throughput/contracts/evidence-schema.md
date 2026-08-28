# Evidence Schema Contract

## Required promoted files

```text
campaign-manifest.json
source-checksums.log
sif-checksum.log
nodes.json
network.json
schedule.json
run-records.jsonl
progress.jsonl
derived-results.json
independent-verification.json
result-terminal.json
checksums.sha256
```

## Run invariants

- One unique `runId` for every scheduled warmup and measured run.
- `pairId` identifies a matched size/repetition observation.
- Terminal state is exactly one of PASS or FAIL.
- Successful transfers include the expected and observed full-object digest.
- Failed transfers include a stable failure code and nonempty reason.
- Every path field is absolute and passes rank-local path isolation.
- Cold retrieval and warm reuse occupy distinct fields and cache states.
- Goodput derives from logical bytes and measured elapsed time; it is never
  copied from NFD or interface counters.

## Security and sanitation

Promoted evidence MUST NOT contain private keys, bootstrap tokens, plaintext
credentials, payload copies, repository databases, or scratch directories.
The checksum manifest covers every retained file except itself.
