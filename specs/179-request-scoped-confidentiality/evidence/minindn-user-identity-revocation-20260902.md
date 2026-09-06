# Spec179 MiniNDN user-identity revocation probe

## Status

Measured on `UAV-Experimental` with the current role binaries and the checked-
in MiniNDN launcher. This is one representative normal-mode cross-process
scenario, not completion of the Spec179 matrix.

## Command

```text
sudo -n python3 tests/minindn/run_request_scoped_confidentiality.py \
  --execute --scenario user-identity-revocation --lifetime-ms 12000 \
  --output /tmp/spec179-mcast-evidence2.QTOfv1
```

The launcher used the seven-node topology, multicast routing for the shared
SVS group prefix, two Users, two Providers, scheduled Controller withdrawal,
the normal Request/ACK/Selection/Response path, and disabled adaptive
admission control. The run completed with zero launcher/process errors.

## Observed result

| Evidence | Value |
|---|---:|
| `gatePassed` | `true` |
| `networkEvidence` | `true` |
| validated Request publications | 156 |
| validated Selection publications | 76 |
| validated Response publications | 31 |
| Provider executions (`EXECUTION_DONE`, unique provider/request) | 16 |
| Controller revocation applied | `true` |
| Controller version | generation `1788384620539`, epoch `2` |
| refresh-attempt log count | 17 |
| terminal owner | `/example/hello/provider/B` |
| terminal reason | `response_callback` |

The raw redacted result and trace hash are retained at the output path above;
the path is temporary evidence and must not be treated as a permanent result
directory. `redactedTraceHash` was
`sha256:a7d086fe33686e716d450aad1a59092329c9a36502fbb2805dafa5606aef518f`.

## Interpretation and limit

This closes one real normal-mode cross-process execution slice and verifies
that the harness can observe Provider execution. It does not establish
certificate-only or service-scoped unaffected controls, restart/offline
recovery, Targeted/refill, large-response, streaming, or the complete
MiniNDN matrix required by T011/T013.
