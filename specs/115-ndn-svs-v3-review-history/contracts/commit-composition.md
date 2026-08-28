# Contract: Complete V3 Commit Composition

## Required owner

Exactly one replacement commit owns official SVS V3 behavior. Its subject is
review-oriented and must identify standards-compliant V3 sync protocol work.

## Required production behavior

- V3 version component and parameters digest naming;
- embedded signed StateVector Data in ApplicationParameters;
- canonical StateVector and BootstrapTime encoding/decoding;
- generic Core, PubSub, and MappingProvider integration required by those V3
  semantics;
- signature/name/content validation before semantic decode;
- atomic rejection with no state or extension mutation;
- one codec shared by serial and parallel production/receive paths;
- V3 timer, immediate publication, suppression, and zero Sync-Ack behavior;
- explicit isolated V2 profile with no hybrid profile.

## Required directly coupled tests

- fixed V2 and V3 wire vectors;
- malformed name/content/signature/digest/vector cases;
- multi-epoch and BootstrapTime cases;
- serial/parallel byte and state equivalence;
- invalid-then-valid state-safety sequence;
- V3 timing and zero Sync-Ack checks.

## Explicit exclusions

- Mapping/Repair fork-extension semantics;
- segmented publication recovery and transactional publication;
- independent NDNts peer and orchestration implementation;
- NDNSF consumer code.

## Machine-checkable invariants

1. Every `official-v3` ownership-manifest row names the same target OID.
2. No `fork-extension` or `reliability` row names that OID.
3. Checking out the OID alone builds and passes the required focused gates.
4. No descendant commit is classified `official-v3-repair`.
5. `git show --stat` and the manifest agree on all required paths.
