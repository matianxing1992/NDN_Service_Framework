# Traceability: UAV Stream Session-Key Delivery

| Requirement | Owning tasks | Primary evidence |
|---|---|---|
| FR-001 through FR-004 | T001, T002, T004 | key-bearing response isolation and descriptor tests |
| FR-003 | T001, T002 | complete descriptor and frontier contract |
| FR-005 through FR-009 | T001, T002, T004 | RNG/nonce/AAD/envelope and encrypted publish vectors |
| FR-007 | T001, T002, T004 | cursor-derived nonce uniqueness and reuse rejection |
| FR-010 through FR-012 | Spec119-T003/T004, T002, T003, T004 | Provider validation, permission, and callback admission |
| FR-011 | T002, T004 | protected key-bearing response and plaintext rejection |
| FR-013, FR-014 | T004, T005 | rekey/restart and secret-scan evidence |
| FR-015, FR-016 | T002, T003, T004 | pending-response and idempotent start tests |
| FR-017 through FR-019 | Spec119-T003/T004, T002, T003, T005 | semantic names, API integration, non-regression, MiniNDN |
| FR-020 through FR-025 | Spec119-T003/T004, T001 through T005 | exact Interest/cache policy, handle lifecycle/PIT bounds, one-time ciphertext, opaque FEC boundary |
| SC-001, SC-002 | T002, T003, T004, T005 | end-to-end and negative callbacks |
| SC-003, SC-004 | T001, T002, T004 | contract, Mapping, and nonce gates |
| SC-005, SC-006 | T004, T005 | rotation and secret scan |
| SC-007 | T001 through T005 | generic NDNSF regressions |
| SC-008 | T005 | two preserved 60-second cells |
| SC-009 | Spec119-T003/T004, T002 through T005 | ciphertext-only FEC, one-loss recovery, duplicate-XOR removal |

## Scope Trace

| Source intent | Enforced by |
|---|---|
| Only successful camera start contains a key | FR-001/FR-004, T002/T004 |
| Direct symmetric encryption of live data | FR-005/FR-009, T001/T002/T003 |
| Keep crypto/media semantics in UAV; generic optional FEC in NDNSF | FR-019/FR-023 through FR-025, Spec119-T003/T004, T002/T003 |
| Keep original meaningful Data names | FR-008/FR-010/FR-018, Spec119-T003/T004, T002/T003 |
| Use signed ahead Mapping, never nested Data | FR-017/FR-018, Spec119-T003/T004, T002/T005 |
| Use one reusable NDNSF prefetch API | FR-019/FR-021, Spec119-T003/T004, T002-T005 |
| Never reuse a Data name or nonce | FR-006/FR-018/FR-022, T001/T002/T004/T005 |
| Bind roots to the expected Provider | FR-010/FR-021, Spec119-T003/T004, T003/T004 |
| Keep generic Response unchanged and avoid another RPC | FR-002/FR-019, T001/T005 |
| Permission is service-scoped | FR-012/FR-013, T004/T005 |
| Encrypt before Core and decrypt after Core | FR-009/FR-023/FR-024, T002-T005 |
