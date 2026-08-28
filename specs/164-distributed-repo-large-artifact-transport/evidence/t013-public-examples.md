# T013 Public Artifact Applications and Documentation Evidence

Date: 2026-07-30  
Verdict: **PASS**  
Claim level: implemented and executed; no performance claim

## Acceptance result

SC-009 is satisfied by the maintained `artifact-manifest-v2` application path:

- one public `publish_file(...)` call publishes a file and returns an immutable
  `ArtifactReference` plus authenticated durability results;
- one public `fetch_file(...)` call verifies and atomically exposes a
  destination;
- async calls, bounded progress dispatch, cancellation, exact-key resume, and
  explicit replica count are available without private runtime access;
- `begin_upload(...)` and `begin_fetch(...)` expose the advanced public session
  lifecycle;
- the maintained publisher and fetcher contain no private client access,
  packet-batch construction, or replica-internal calls;
- English and Chinese READMEs document every argument, result field, stable
  error code, Collaboration/Targeted control choice, and the explicit
  `exact-packet-v1` compatibility boundary.

The post-implementation audit found and fixed two bounded-memory defects in the
first example draft: whole-file `read_bytes()` hashing and an unbounded progress
list. The canonical publisher now computes SHA-256 in 1 MiB blocks, and both
applications retain only a constant-space progress counter.

## Public applications

- `examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py`
- `examples/python/NDNSF-DistributedRepo/artifact_api/fetch_file.py`

The local backend used by these standalone examples is
`FilesystemArtifactApiBackend`. It is a crash-safe, persistent, single-replica
backend, not a substitute for the deployment-owned NDNSF network adapter.
`Experiments/NDNSF_DistributedRepo_PublicApi_Minindn.py` calls the same public
facade while its backend adapter owns the real MiniNDN transport setup.

## Automated contract tests

Command:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec164*.py'
```

Result: **65/65 passed**.

Coverage relevant to T013 includes:

- public sync and async publication/retrieval;
- stable idempotency and two-replica result/receipt accounting;
- progress monotonicity and bounded observer isolation;
- cancellation and deterministic error mapping;
- persistent local deduplication, destination conflict/replace, atomic fetch,
  interrupted publication, and verified-prefix resume;
- executable advanced-publish plus async-fetch application smoke;
- source guard against private fields, packet helpers, and replica internals;
- synchronized English/Chinese public-contract terms.

## Canonical local smoke

Path:

```text
specs/164-distributed-repo-large-artifact-transport/evidence/us3/local-public-api-20260730T041350Z/
```

The advanced publisher and async fetcher ran as separate processes over a
262,144-byte file. Publication achieved one durable receipt; retrieval
transferred 262,144 bytes; `cmp` verified exact reconstruction. The retained
directory contains only `reference.json`, `publish-result.json`, and
`fetch-result.json`; the source, destination, staging data, and local CAS were
removed.

## Canonical MiniNDN smoke

Command:

```bash
sudo -n env \
  PYTHONPATH="$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  python3 Experiments/NDNSF_DistributedRepo_PublicApi_Minindn.py \
  --output-dir \
    specs/164-distributed-repo-large-artifact-transport/evidence/us3/minindn-public-api-20260730T041225Z \
  --payload-size 32768 \
  --timeout-seconds 30
```

Path:

```text
specs/164-distributed-repo-large-artifact-transport/evidence/us3/minindn-public-api-20260730T041225Z/
```

Result: **PASS**.

- publisher, Repo, and consumer ran as distinct MiniNDN nodes;
- publication used 8 Interests for 8 segments, 32,768 logical bytes, 35,992
  wire bytes, and zero retransmissions;
- retrieval used 8 Interests for 8 segments, 32,768 logical bytes, 35,912 wire
  bytes, and zero retransmissions;
- the publication returned one distinct authenticated `COMMITTED` receipt;
- the consumer exposed the destination only after full validation;
- the public facade observed three progress events;
- payload, staging, key, local object store, and reconstructed destination were
  removed after verification;
- `performanceClaim` is explicitly `false`; the measured 7,120.290 ms is smoke
  timing, not a throughput result.

## Compatibility boundary

Legacy `exact-packet-v1` tests intentionally exercise the explicit packet-level
compatibility API because preserving application-signed Data wire bytes is
their subject. They are not maintained `artifact-manifest-v2` applications and
are not a silent fallback for `publish_file(...)` or `fetch_file(...)`.

## Gate audit

- Context Mode: optional retrieval was attempted; repository guard remained
  fail-closed because no project ContentDB was bound, so repository artifacts
  were authoritative.
- CodeGraph: verified the public facade, local backend, examples, result types,
  and test blast radius.
- Spec Kit: strict structure scan passed before closure.
- GSD: previously validated workflow state remains the resumable outer loop.
- ARS: no performance inference was made from functional smoke evidence.

No unresolved CRITICAL, HIGH, or MEDIUM T013 finding remains. Gate verdict:
**PASS**.
