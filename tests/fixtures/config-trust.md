# Configuration parser trust fixture

`config-trust.conf` is a small public-only anchor for configuration parsing tests.
It has no production identity or private key. Certificate subject:
`/ndnsf/test-only/config-anchor`; validity: 2000-01-01 through 2126-01-01.
The native `ndnsec key-gen -t r` and `cert-gen -S 20000101000000 -E 21260101000000`
created it in an isolated temporary PIB/TPM; all private state was discarded.

Runtime actor tests create their own identities and `IsolatedTestTrust` instead.
This fixture cannot supply their signing keys or replace their deployment credentials.
