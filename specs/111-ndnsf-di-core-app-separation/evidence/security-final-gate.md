# Security Final Gate

Date: 2026-07-14  
Verdict: **PASS (local characterization); distributed acceptance remains MiniNDN-only**

The post-remediation `./examples/run_security_regressions.sh` completed in 70
seconds with all six
frozen leaf markers and the aggregate marker:

- HELLO authorization: 1 PASS
- authenticated ACK payload: 1 PASS
- selective custom selection: PASS (marker is printed by leaf and aggregate)
- NAC-ABE attribute routing: 1 PASS
- negative token handshake: 1 PASS
- certificate bootstrap/reuse: 1 PASS
- `NDNSF_SECURITY_REGRESSIONS=PASS`: 1
- production `isAuthorized = true` bypass matches: 0

Raw log: `/tmp/spec111-final-r2/security.log`, SHA-256
`b329ea1131ff47f34b462301568148c1fcb45b45f2317377a174f91cd9193735`.
This frozen local security characterization is not cited as a distributed
network result; Spec 111 distributed/fault/performance evidence uses MiniNDN.
