# Tiger D0 job 199484: stale SIF CLI negative evidence

Job `199484` is **not** a protocol or CUDA result. It is an invalid-candidate
failure discovered by the real multi-Provider workload:

```text
SIF: /project/tma1/ndnsf-di/releases/spec170-runtime-31d9f547-post-selection-v2-20260817/runtime.sif
workload: network-bundle-d0-cpu-r3-bounded-startup/spec170-d0-current-sif-workload.sh
```

The workload passed `--bootstrap-token` to each `di-native-provider`. The
Provider binary inside this SIF printed a usage line that did **not** contain
`--bootstrap-token`, then failed before the request phase. The preserved log
also reported:

```text
error: could not connect to NDN forwarder at /scratch/run/nfd.sock
```

The socket message is secondary: the executable had already rejected a CLI
option that the current source/build accepts. The same SIF was independently
probed on Tiger and its usage output confirmed the stale CLI surface. The
candidate was built before the current Provider parser change was embedded;
the current source at `HEAD=31d9f547a32f941ab948f005f583c429d23bacbb` contains
the option in `examples/DI_NativeProviderExecutable.cpp`.

Classification: `INVALID_CANDIDATE`; do not rerun job 199484 or modify its SIF
in place. Build a new local application SIF from the current sealed source,
run the local real NativeTracer D0/D1 gate, and probe the exact SIF's Provider
usage/runner closure before a new Tiger submission. The local MiniNDN and
in-process tests passing do not promote this stale SIF.
