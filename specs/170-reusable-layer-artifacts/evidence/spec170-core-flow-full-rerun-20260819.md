# Spec170 full native core-flow rerun (2026-08-19)

The complete current-build `Spec170NdnsfDiCoreFlow` suite was rerun to cover
the production paths beyond the focused post-Selection subset.

```text
command: build/integration-tests --run_test=Spec170NdnsfDiCoreFlow --log_level=test_suite
return code: 0
result: 26 test cases; no errors detected
elapsed: 19.15 s
captured log: 13,365 bytes
log sha256: 1054b58111e4b3a4a9b4b134808d232befc2987339d7065dc5415852eafc0527
```

The passing suite includes:

- request → Selection → assignment → Response ingress;
- D2a two-device execution;
- D2b SVS DATA_V1 positive, tamper, drop, duplicate, and reorder cases;
- native capability tamper rejection;
- D2h `[1,2,1]`, `[2,1,2]`, and frozen heterogeneous mappings;
- four-Provider role-split request/Selection/Response.

This strengthens current-build protocol and named-negative qualification. It
does not close the full T028/T037 mutation corpus, T029 source/SIF freeze, or
T036 statistical performance-optimality evidence.
