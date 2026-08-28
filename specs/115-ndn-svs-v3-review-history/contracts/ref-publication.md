# Contract: Ref Publication and Rollback

## Required inputs

- freshly fetched expected old `origin/master` OID;
- candidate master OID and replacement V3 OID;
- candidate Experimental OID;
- verified local and remote backup names;
- completed pre-publication validation records;
- explicit execution authorization.

## Precondition

Immediately before publication, remote `refs/heads/master` must equal the
recorded expected old OID. Any mismatch aborts publication and invalidates the
lease; it must not trigger an automatic retry.

## Publication order

1. Create the local immutable backup.
2. Publish a uniquely named remote backup at the old OID.
3. Verify that remote backup through an independent remote-ref query.
4. Recheck the remote master fence.
5. Update remote master with an explicit expected-old-OID force-with-lease.
6. Fetch remote state again.
7. Verify local/remote master equality, V3 containment, PR head, and backup.
8. Update PR review description/evidence only after branch verification.

Unqualified force push is prohibited. `upstream/*`, tags, and releases are
outside the publication contract.

## Completion invariants

```text
master == origin/master == PR36.head
replacementV3Oid ancestor-of master
replacementV3Oid ancestor-of origin/master
replacementV3Oid ancestor-of Experimental
remoteBackup == oldOriginMasterOid
```

## Rollback

Rollback is allowed only when the current remote head still equals the recorded
published candidate. Restoration uses the remote backup OID and an explicit
candidate-OID lease. After rollback, a fresh fetch must prove PR head and local
master restoration. Safety refs are not deleted by this feature.
