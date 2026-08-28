# Artifact Metadata Migration and Rollback Contract

## Scope

This contract governs `artifact-manifest-v2` metadata and payload compatibility.
It does not reinterpret, rename, resign, or migrate `exact-packet-v1` Data
wires. Legacy exact-packet names, signatures, and stored wire bytes retain their
existing trust semantics.

## Durable Schema Generation

Each artifact metadata database stores one monotonically increasing schema
generation. Runtime startup compares:

- the database generation;
- the runtime generation;
- the operator-selected maximum write generation; and
- the operator write-enable flag.

A missing generation on an existing artifact lifecycle database denotes the
documented generation-11 predecessor. A new empty database denotes generation
zero.

## Non-Destructive Roll-Forward

An understood older generation is upgraded additively. Generation 12 adds
explicit `format_version` and `digest_algorithm` identity to active catalog,
authenticated receipt, and garbage-collection metadata. Existing values are
derived only from their already-retained canonical artifact/receipt metadata;
payload files and signed manifest bytes are not rewritten.

Automatic startup migration MUST NOT delete payloads, receipts, catalog rows,
or legacy exact packets. Any destructive conversion is a separate,
operator-authorized workflow outside Spec 164.

## Fail-Closed Rollback

Artifact writes are disabled when:

- the operator disables v2 publication;
- the database generation exceeds the runtime generation; or
- the database generation exceeds the operator's maximum write generation.

In that mode, committed artifacts, authenticated receipts, and payload bytes
remain readable. Startup recovery, finalization replay, queued-task or transfer
session mutation, legacy reservation-row mutation, resume checkpoint mutation,
and garbage collection do not run. Every attempted v2 mutation fails with
`repo-artifact-writes-disabled`.

The node withdraws `artifact-manifest-v2`, resume, and replica-receipt support
from capability advertisements while retaining explicit `exact-packet-v1`
compatibility. Consequently mixed-version placement rejects the node before
publication rather than failing after transfer.

## Format Identity

The following identity is retained through catalog, receipt, GC, migration, and
rollback:

```text
(formatVersion, digestAlgorithm, contentDigest, storageGeneration)
```

Logical-name lookup additionally requires `policyEpoch`. GC ownership and
protection checks match the complete format identity and cannot claim an active
or receipted generation.

## Operator Diagnostics

ACK and `CAPABILITY` responses expose bounded `artifactMigration` fields:

- `runtimeSchemaGeneration`;
- `databaseSchemaGeneration`;
- `previousSchemaGeneration`;
- `maxWriteSchemaGeneration`;
- `action`: `initialized`, `none`, `roll-forward`, or
  `read-only-rollback`;
- `writesEnabled`;
- `reason`; and
- `destructiveChanges`, which is `false` for every automatic path.

These diagnostics contain no peer-controlled raw database text, filesystem
path, key material, payload, or signature.
