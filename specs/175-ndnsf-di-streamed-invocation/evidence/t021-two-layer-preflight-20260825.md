# T021 two-layer preflight correction — 2026-08-25

## Scope

This checkpoint corrects the preflight boundary only. It does not rebuild or
promote a SIF and does not claim G4 replay or Tiger success.

The exact-SIF probe now checks only SIF-owned runtime state:

- CPython 3.10 and `_ndnsf.so` import;
- Provider/framework `ldd` closure;
- ONNX Runtime CUDA provider;
- NFD, `nfdc`, `ndnsec`, Controller, and Provider entry points;
- `ndnsf`, `py_repoclient`, and `ndnsf_distributed_inference` imports;
- absence of deployed PyTorch/Transformers/functorch.

It no longer searches for or imports `mn`, `mnexec`, `ovs-*`, `ip`, `nlsr`,
MiniNDN, or Mininet.

The new host-layer command is
`packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight`. It checks
the host MiniNDN/Mininet/OVS/NLSR command and Python-module closure, topology,
root namespace privilege, Apptainer 1.5.3, and the host replay driver.

## Verification

```text
skill-creator quick_validate: PASS
pytest focused Spec175/source/preflight/host-gate tests: 34 passed
exact-SIF runtime preflight against the existing candidate: PASS
host-substrate preflight as the unprivileged developer user: FAIL closed with
  HOST_NAMESPACE_PRIVILEGE_MISSING
```

The host preflight failure is expected outside `sudo`/root and is not a G4
failure. It proves the gate refuses to launch a namespace replay without the
required privilege.

The exact-SIF diagnostic used the existing candidate
`sha256:932672c8aef10800a854c859b4f3b05c10184f5381323956829570d99d560c17`
and existing source seal
`sha256:284554c451f912e815c358446a30faa524c5e07e71504720b314f0ae979a7891`.
The JSON output hash was
`sha256:e20123d9f89bc88266cc4c0dc7ba18b8b164dfaaf6c52ffa37c51f1d42494a87`.
This is diagnostic validation of that sealed candidate, not evidence for the
current dirty checkout.

## Remaining gate

The host replay driver now fails closed until the production MiniNDN runner
actually consumes `SPEC175_RUNTIME_SIF` and launches each NFD/application
process through `apptainer exec --cleanenv`. No host-runtime fallback is
allowed. T023/G4 therefore remains open until that command-provider path and
the 30-case exact-SIF replay are implemented and executed.
