# T011 diagnostic path-preflight failure — 2026-09-05 r40 I-only

## Disposition

`UNQUALIFIED` preflight/diagnostic interruption only. The intended I-only
live observation did not produce a valid result because the command used a
mistyped temporary root outside the repository and was interrupted before the
maintained helper completed. It is not evidence about Controller readiness,
Y-N-I, or ACK disposition.

## Exact issue and preservation

The command used `/home/tianxing/NDN/ndns-service-framework` instead of the
repository path `/home/tianxing/NDN/ndn-service-framework`. The run was
interrupted with exit status `130` after MiniNDN printed its ambient resource
limit diagnostic. The explicitly created temporary directory was moved into
the repository as `spec180-diagnostic-path-error-r40` so the partial raw data
is preserved and is not confused with a completed run.

No structured subcase marker, readiness closure, SIF command, or TigerCluster
command was produced.

## Next action

Use the exact repository-root path and a new run-id for the I-only startup
trace. This path error is closed by preserving the partial data; it does not
alter the active r39 startup/transport blocker.
