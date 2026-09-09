# Exact-SIF MiniNDN Y-N matrix: v49 / v31 application

**Run:** `minindn-local-20260909-v49-yn46`\
**Date:** 2026-09-09\
**Scope:** local exact-SIF CPU registered negative matrix

The run used the same base SIF hash
`sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`,
external application manifest hash
`sha256:39ca5bd3e8d33598145229ce9dd9bcbbb58903cb20eb110fd16443c1fd13a513`,
and source revision `c046778adc618d108dbaff23e82551fb85ea9447`.  The maintained
Y-N owner returned `T010_DONE` with return code 0, `processCleanup=CLEAN`, and
an empty supervisor error list.

All seven registered subcases passed their own fail-closed checks:

| Subcase | Observed boundary | Result |
| --- | --- | --- |
| Y-N-O | `TERMINAL_RESPONSE` | control response passed |
| Y-N-C | `PLACEMENT_DECISION` | `NO_FEASIBLE_CANDIDATE` |
| Y-N-P | `ACK_CLOSED` | `ACK_PROVENANCE_REJECTED` |
| Y-N-R | `PLAN_SEALED` | `ROLE_KIND_REJECTED` |
| Y-N-I | `PROVIDER_EXECUTION_STARTED` | `NON_INGRESS_INPUT_REJECTED` |
| Y-N-E (three variants) | `PROVIDER_GRANT_VERIFICATION` | `DI_PROTECTED_GRANT_REJECTED` |
| Y-N-L | `EVIDENCE_ACCEPTANCE` | `REDACTION_REJECTED` |

The three Y-N-E variants provide real provider-bound protected-grant rejection
records and are suitable as the permission-rejection input to the host-gate
producer.  Y-N-C is deliberately **not** a valid Spec183
`negative-dependency` record: it fails before Selection at placement, whereas
the contract requires a missing dependency or peer failure after Selection.
Therefore this run closes the registered mutation matrix but leaves the host
negative-dependency case and the combined host qualification receipt open.
