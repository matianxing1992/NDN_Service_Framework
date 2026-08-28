# Data Model: Spec 128

## RetryEligibility

Immutable cursor/name, attempt count, retry horizon, active generation, and
terminal disposition. A retry is valid only while all five remain valid.

## RecoveryDeclaration

Signed group identity, source names/cursors/lengths/digests, scheme, declared
capacity, repair-symbol index, repair name/cursor, and expiry. All inputs are
validated before recovery.

## RecoveryAttempt

Group identity, missing count, declared capacity, selected repair indices, and
one of recovered/insufficient/invalid/expired/capacity-exceeded/rejected.

## Relationship

Retries are cursor scoped; recovery is group scoped. A group may atomically
complete several missing cursors only after each reconstructed opaque byte
sequence matches its authenticated length and digest.
