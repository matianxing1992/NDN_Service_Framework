# T015 Migration, Rollback, and Mixed-Version Evidence

Verdict: **PASS**

## Implemented contract

- Native and Python metadata runtimes now persist schema generation 12.
- Generation-11 metadata rolls forward additively.
- Active catalog, authenticated receipt, and GC metadata retain
  `formatVersion` plus `digestAlgorithm`.
- Automatic migration does not rewrite committed payload or exact-packet bytes.
- Operator-disabled, maximum-write-generation, and unknown-future-generation
  startup paths disable all new v2 metadata mutation.
- Read-only rollback retains committed artifact and authenticated receipt reads.
- Read-only nodes withdraw v2, resume, and replica-receipt capability while
  preserving explicit `exact-packet-v1`.
- ACK and `CAPABILITY` responses expose bounded `artifactMigration`
  diagnostics.

Machine-readable evidence:
[`us4/migration-rollback-20260730T044500Z/summary.json`](us4/migration-rollback-20260730T044500Z/summary.json)

## Focused verification

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:tests/python \
  python3 tests/python/test_spec164_migration_rollback.py
PASS: 5/5
```

The five cases cover additive v11-to-v12 migration, v12 rollback read/write
separation, operator-disabled v11 reads without migration, future-generation
fail-closed behavior, and operator-visible capability withdrawal.

```text
./waf --targets=unit-tests -j2
PASS

./build/unit-tests \
  --run_test=DistributedRepoFilesystemArtifactStore \
  --log_level=test_suite
PASS: 5/5
```

The native backend suite includes additive generation-9-to-12 roll-forward,
retained lifecycle rows, unknown-generation read-only startup, and a stable
`repo-artifact-writes-disabled` mutation failure.

## Regression verification

```text
for test_file in tests/python/test_spec164_*.py; do
  python3 "$test_file"
done
PASS: 77/77

python3 tests/python/test_ndnsf_repo_exact_packets.py
PASS: 12/12

python3 tests/python/test_ndnsf_repo_ha.py
PASS: 48/48

python3 tools/maintenance/ndnsf_occam_audit.py . \
  --rule obsolete-repo-surface --json --fail-on-active
PASS: active findings 0
```

The exact-packet suite remains unchanged: names, application signatures, wire
bytes, and explicit API separation retain their prior trust semantics.

## SC-010 closure

The frozen predecessor is readable, the understood predecessor rolls forward
without payload conversion, future or operator-incompatible schemas are
read-only, and committed objects survive rollback byte-for-byte. Incapable
peers fail during capability negotiation rather than after transfer.
