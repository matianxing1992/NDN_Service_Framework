# NAC-ABE large-data MiniNDN gate (2026-08-19)

This is a protocol-correctness gate, not a performance campaign. It uses the
current local build and the existing wired `Experiments/Topology/AI_Lab.conf`
topology; it does not build or copy a SIF.

Command:

```text
sudo -n python3 Experiments/NDNSF_LargeData_NacAbe_Minindn.py \
  --output-dir results/spec170-large-data-minindn-20260819
```

The runner installs root-signed RSA/ECDSA example identities, starts the
Controller and an authorized `/example/hello/provider`, and publishes an
8,217-byte deterministic plaintext through the User. The User log records two
encrypted segments. The bootstrap Provider is then replaced by an authorized
fetch Provider and a separate `/example/hello/provider/unauthorized` Provider;
the latter is controller-authorized only for `OTHER`, not `HELLO`.

Result: `PASS` in
`results/spec170-large-data-minindn-20260819/summary.json`.

| Check | Result |
|---|---|
| User publishes encrypted data | PASS; `segments=2`, `activePut=true` |
| Authorized Provider reconstructs exact plaintext | PASS; `LARGE_DATA_FETCH_SUCCESS` |
| Unauthorized Provider receives no HELLO plaintext | PASS; `LARGE_DATA_UNAUTHORIZED_FAILURE_CLEAN` |
| Adaptive admission | Disabled |
| Harness | MiniNDN with per-node NFD; no host-NFD substitution |

The unauthorized failure is bounded by the Provider fetch helper and is
reported as a clean hybrid-decrypt/permission failure. The prior host-NFD run
remains diagnostic evidence only; this MiniNDN run is the formal large-data
integration gate for this path.

