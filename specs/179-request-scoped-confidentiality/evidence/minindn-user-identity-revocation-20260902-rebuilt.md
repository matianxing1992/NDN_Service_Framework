# MiniNDN user-identity revocation (rebuilt source)

Date: 2026-09-02  
Branch: `UAV-Experimental`  
Scenario: `user-identity-revocation`  
Mode: normal Request/ACK/Selection/Response  
Recovery: none

Command:

```bash
sudo -n timeout 90s python3 tests/minindn/run_request_scoped_confidentiality.py \
  --execute --scenario user-identity-revocation --lifetime-ms 12000 \
  --output /tmp/spec179-user.FdXErg
```

The run used the rebuilt `build-clang-nodbg` role binaries. All five role
processes exited successfully and the launcher produced real MiniNDN packet
evidence; no synthetic result was generated.

| Field | Observed value |
|---|---:|
| `gatePassed` | `true` |
| `networkEvidence` | `true` |
| ControllerVersion | generation `1788387982617`, epoch `2` |
| Request publications | `156` |
| Selection publications | `76` |
| Response publications | `31` |
| unique provider/request executions | `16` |
| refresh/status observations | `17` |
| revocation applied | `true` |
| terminal owner | `/example/hello/provider/A` |
| terminal reason | `response_callback` |
| redacted trace hash | `sha256:7d14b7e95c5304239372e020f842582d348f65d31096d4212b3390d28609d2a9` |

This is one normal-mode identity-revocation network result. It does not close
the provider-restart, offline/rejoin, large-response, Targeted, stream, cache-
source, or Controller-unavailable rows in the Spec179 matrix.
