# Phase 2 Security Characterization Baseline

Date frozen: 2026-07-14  
Source revision: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6` plus the pre-movement Spec 111 worktree.

This is the exact existing security regression matrix used before source
movement. The aggregate command is:

```bash
./examples/run_security_regressions.sh
```

The aggregate starts a temporary host NFD only when `/run/nfd/nfd.sock` is not
already available and stops only the NFD it started. It is characterization,
not final distributed acceptance; Spec 111 final network/security evidence is
MiniNDN-only.

| # | Script | Required PASS marker | Characterized invariant |
|---:|---|---|---|
| 1 | `examples/run_hello_auth_regression.sh` | `HELLO_AUTH_REGRESSION=PASS` | encrypted controller permission replies, controller signatures, user/provider permissions, certificate selection, UserToken/ProviderToken mode, HELLO response, no authorization bypass |
| 2 | `examples/run_hello_ack_payload_regression.sh` | `HELLO_ACK_PAYLOAD_REGRESSION=PASS` | authenticated ACK metadata and selected execution, no authorization bypass |
| 3 | `examples/run_selective_ack_custom_selection_regression.sh` | `SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS` | three ACK candidates, rejected C, timeout-driven selection of B, exactly B emits a final response |
| 4 | `examples/run_nac_abe_attribute_routing_regression.sh` | `NAC_ABE_ATTRIBUTE_ROUTING_REGRESSION=PASS` | REQUEST/SELECTION use `/SERVICE/HELLO`; ACK/RESPONSE use `/PERMISSION/HELLO` |
| 5 | `examples/run_token_handshake_negative_regression.sh` | `TOKEN_HANDSHAKE_NEGATIVE_REGRESSION=PASS` | wrong UserToken, wrong ProviderToken and replayed ProviderToken are rejected |
| 6 | `examples/run_token_certificate_bootstrap_regression.sh` | `TOKEN_CERTIFICATE_BOOTSTRAP_REGRESSION=PASS` | encrypted name-bound bootstrap, proof/name/tamper rejection and valid identity-token reuse |

Expected count: **6/6 leaf PASS markers**, **1/1 aggregate
`NDNSF_SECURITY_REGRESSIONS=PASS` marker**, zero unexplained exits, and zero
matches for a production `isAuthorized = true` bypass in the generated logs.
Negative rejection checks inside a leaf script are part of that leaf result and
must not be counted as failures.

