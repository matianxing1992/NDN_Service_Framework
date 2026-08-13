# TigerCluster Spec170 storage probe (2026-08-13)

The `/project` filesystem reports ample shared capacity, but the `tma1`
project allocation is at its quota boundary. A read-only `du` probe of
`/project/tma1/ndnsf-di` reported approximately:

```text
artifacts 105G
releases   37G
evidence  7.8G
inputs    4.3G
jobs      1.3G
src       1.3G
images     35G
total    ~193G
```

The first attempted full source archive transfer exceeded that quota and was
preserved as a failed transfer under Tiger `/tmp`; no existing release,
artifact, or evidence directory was deleted. The verified 6.5-MB source
subset was then staged successfully under
`candidates/spec170-source-fb49fd9`.

Before retaining an OCI-derived SIF for Gate C or any D gate, an operator must
choose an explicit, recoverable retention action for superseded artifacts or
provide additional project quota. Shared filesystem free space is not a
substitute for the project quota.
