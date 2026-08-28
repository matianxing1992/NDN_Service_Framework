# Contract: Fidelity Records and Aggregate Verdict

## Fidelity tiers

Ordered tiers are:

1. `STATIC`
2. `UNIT`
3. `FIXTURE`
4. `HOST_PROCESS`
5. `REAL_MININDN_MODEL`
6. `REAL_CANDIDATE_CONTAINER_MODEL`
7. `REMOTE_MULTI_NODE`

Higher order does not imply substitution unless policy explicitly permits it.
Gate B requires `REAL_MININDN_MODEL`; Gate C requires
`REAL_CANDIDATE_CONTAINER_MODEL`.

## Required record

Each case emits exactly one `FidelityRecord` described in `data-model.md`.
The record lists real and simulated components independently. Empty or
contradictory declarations are invalid.

## Aggregate rules

The aggregate:

1. loads only records referenced by the current run manifest;
2. validates schema and policy digests;
3. requires matching run ID and source revision;
4. requires exact model/workload identity for Gates B and C;
5. rejects duplicated mandatory case IDs;
6. treats `SKIP`, timeout, missing output, and malformed output as failure;
7. never promotes a lower-fidelity record;
8. derives JSON and Markdown summaries from one verdict object.

An optional diagnostic case may skip without changing mandatory accounting,
but its skip remains visible.

## Exit codes

- `0`: all requested mandatory cases passed.
- `1`: one or more cases ran and failed.
- `2`: configuration, prerequisite, schema, or evidence integrity failure.
- `3`: interrupted or infrastructure error with incomplete accounting.

Unknown exit codes are aggregate errors.
