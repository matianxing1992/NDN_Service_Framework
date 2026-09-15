# Spec186 current-source native build r64

## Scope

This is a host-local native build receipt for the current source candidate. It
does not provide a container ABI, MiniNDN, CUDA, or Tiger qualification
receipt. The host output is retained only to prove that the current source can
be compiled and inspected before rebuilding the matching container bundle.

| Field | Result |
| --- | --- |
| source checkout | `SPEC184Experiments` at `2a36aba7` |
| build directory | `build-spec186-r64` |
| builder | host Waf + Python 3.8 extension build |
| native targets | `197/197`, Waf build exit 0, 14m30.116s |
| parallelism | Waf `-j4`; Python extension is one serialized translation unit |
| native identity | `SPEC180_NATIVE_IDENTITY_OK build-spec186-r64/spec180-native-build.json` |
| official verify | `python3 scripts/spec180_native_build.py verify --build-dir build-spec186-r64`, exit 0 |
| Python import | `import ndnsf._ndnsf` selects `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so` |

## Loader and entrypoint checks

`ldd -r` returned exit 0 with no `not found` or native undefined-symbol rows
for `libndn-service-framework.so`, `libndnsf-distributed-inference.so`,
`DI_NativeOnnxAssemblyWorker`, `DI_NativeRequester`,
`DI_NativeArtifactAuthority`, and `di-native-provider`. The Python extension
also returned exit 0 with 161 expected interpreter-provided Python C-API
undefined symbols and no missing libraries.

`DI_NativeRequester --help`, `DI_NativeArtifactAuthority --help`, and
`di-native-provider --help` returned 0 with usage text. The assembly worker
returned its established invalid-mode status 2 for `--help`; this binary uses
its workload invocation contract rather than a help option.

The host ELF RUNPATHs still contain `/tmp/spec186-nacabe-install-r5/lib`,
`/home/tianxing/NDN/ndn-svs/build`, and the r64 host build directory. Those
paths explain why the host closure cannot be promoted directly into a SIF;
the application bundle must be rebuilt in the matching container/SDK and
checked against `/opt/ndnsf-di/current`.

## Artifact hashes

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `libndn-service-framework.so` | 21218904 | `0dedcb4404b216923af920a58a0cf1fec08495075fbae37a3531d6155675ae15` |
| `libndnsf-distributed-inference.so` | 44169128 | `32091e0f5046515cb688f45041757afcf6e653780cf6cdaa1bc88dc43a648f70` |
| `DI_NativeOnnxAssemblyWorker` | 7973000 | `2a4b43a7d647ac5260f8fc610b97cf0a5a117076f62a4ff29db9615641f23397` |
| `DI_NativeRequester` | 2627608 | `c3a5837a31fa274497aac94f97ab6e9c62ef6c272b568c069577b627e6538082` |
| `DI_NativeArtifactAuthority` | 1499440 | `1f9e97217c5cb8dff9edccf372c1acd2fd94a5564773dd40dcbf55432d19e134` |
| `di-native-provider` | 47698752 | `358b234eb85ba8357e734f3a895b753145f261f489fadaef81b51d8f653d3342` |
| `_ndnsf.cpython-38-x86_64-linux-gnu.so` | 110725448 | `64d6a5d14fdcb191fdb05b0c08b362c83aa9bf66243ce6562a82e281a885edd0` |

The next closure gate is a source-sealed application rebuild in the matching
Apptainer 1.5.3 builder; the host RUNPATH result above remains a hard boundary.
