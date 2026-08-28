# Spec170 cross-Provider DATA_V1 unit rerun (2026-08-19)

The current build's cross-Provider security/transport unit suite was rerun
independently of the full unit total.

```text
command: build/unit-tests --run_test=DistributedInferenceCrossProviderGroup --log_level=test_suite
return code: 0
result: 9/9 cases; no errors detected
log bytes: 3,569
log sha256: c79ba54f1a9355a305ccad96f9b887270ef574286e3a18f89f82ccf20ac95d89
```

The suite covers bounded operation sealing, RSA-wrapped epoch keys, manifest
mutation, terminal reuse after cancellation, progress/deadline enforcement,
duplicate/replay handling, and the fixed 50-seed DATA_V1 fault matrix. This is
strong unit evidence for the named protocol invariants; production 3A/3B/3C
fault lifecycle coverage and T028/T037 freeze requirements remain open.
