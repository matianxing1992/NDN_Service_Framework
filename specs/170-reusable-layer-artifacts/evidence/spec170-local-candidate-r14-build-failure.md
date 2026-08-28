# Spec170 local candidate r14 build failure

- Status: `FAIL` before compilation
- Definition SHA-256: `sha256:6425a7f48788ee1ed6e9413f5a13a255ccad69072c5d1441be77afd64e463a8b`
- Build boundary validator: `PASS`
- Apptainer: local `1.5.3`, compute contract `1.5.3-1.el9`
- Failure: Waf `configure --with-examples` reported
  `Checking for 'gtkmm-3.0': not found` and exited nonzero.
- Classification: incomplete builder dependency/configuration closure; no SIF
  was produced and no Tiger job was submitted.
- Correction rule: enumerate and probe every mandatory Waf dependency inside
  the builder before configure, retain `container-configure-closure.json`, and
  use a new candidate identity.
