# Quickstart

## Preflight

Run the harness dry-run/preflight and verify that it prints exactly six cells:
gRPC and NSC at 100, 150 and 200 m. It must reject an NDNSF formal cell.

## Focused validation

1. Run focused Python tests for gRPC failover and harness parsing.
2. Build `Experiments/NDN_NSC/consumer` and `producer` with the local Makefile.
3. Run focused NSC state/accounting tests.

## Smoke

Run one gRPC and one NSC MiniNDN smoke with a measured duration below 10
seconds. Require `SMOKE_OK`, a terminal summary, three started Provider
processes and a mobility trace. Smoke output is not performance evidence.

## Formal campaign

Create a unique output directory, show the exact command in the session, and
run the single six-cell campaign. Monitor process liveness, terminal cell
artifacts and output growth. Do not automatically retry any failed cell.

## Completion audit

Verify exactly six unique terminal cells, the common contract fields, 300
scheduled requests per healthy 60-second client run, no NDNSF process/cell,
and unchanged frozen NDNSF input hashes.
