# Contract: NDNSF Mapping and Repair Extension Profile

## Boundary

The standard State Vector Data is always the first complete block in V3
ApplicationParameters. Fork extensions follow it and never change its Name,
Content grammar, or signature.

```text
ApplicationParameters
├── Data(Name=/group/v=3, Content=StateVector, Signature)
├── MappingData?   # existing fork/PS metadata
└── RepairData?    # fork-private bounded recovery request
```

## Ownership

- Core extracts and validates State Vector Data and returns trailing blocks.
- Core does not interpret MappingData or RepairData.
- SVSPubSub owns known-extension decoding, limits, deduplication, and mutation.
- Unknown extension blocks are skipped without rejecting valid core V3 state.
- The extension parser receives the already validated vector only as immutable
  context.

## Atomicity

Known extension handling uses a prepare/commit boundary:

1. parse all instances and validate size/count/range/type constraints;
2. derive proposed mapping/repair operations without mutation;
3. if any known block is malformed, discard all proposed extension operations;
4. otherwise commit them once with existing deduplication rules.

The core vector transition and extension transaction have separate diagnostics.
A malformed extension does not undo valid core convergence, but it applies zero
extension state.

## Unknown and duplicate blocks

- Unknown non-critical extension TLVs are ignored and counted.
- Multiple MappingData or RepairData blocks are rejected unless the profile
  explicitly defines deterministic aggregation; the initial implementation
  permits at most one of each.
- Duplicate repair entries remain subject to Spec 113 suppression/deduplication.
- Length, entry count, and requested ranges remain bounded by existing limits.

## Compression

`LzmaBlock(211)` around the whole ApplicationParameters is not part of this V3
profile. A V3 participant configured with whole-envelope compression fails
construction with an actionable diagnostic. V2 build-time compression behavior
is not changed by this feature.

## Standards claim

Passing this profile means “core SVS V3 compatible with optional NDNSF
extensions.” It never means RepairData or LZMA are standard SVS V3 fields.
