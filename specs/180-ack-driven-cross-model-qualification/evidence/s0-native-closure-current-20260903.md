# S0 Native Closure Evidence

> **INVALIDATED — DO NOT REUSE.** The revision-123 era reworked the native
> evidence and repaired the provider native boundary after this closure ran
> on 2026-09-03 (spec181 R003 banner audit).  The current-source native
> records are `audit-revision123-design-code-conformance-20260904.md`,
> `provider-boundary-repair-20260904.md`, and
> `t011-native-boundary-repair-20260904.md`.  This historical closure PASS
> is retained for audit history only and cannot satisfy a new S0 gate.

Date: 2026-09-03  
Source HEAD: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` plus the recorded
Spec180 worktree changes  
Verdict: **PASS**

## Build

The framework build used the required bounded command:

```bash
./waf -o build-system-j2 build -j2
```

The host Python ABI is CPython 3.8.10. The extension was forcibly rebuilt with
the accepted framework directory as an exclusive native candidate:

```bash
cd pythonWrapper
NDNSF_LIBRARY_DIR=/home/tianxing/NDN/ndn-service-framework/build-system-j2 \
  python3 setup.py build_ext --inplace --force --parallel 2
```

`pythonWrapper/setup.py` now rejects a missing explicit candidate and does not
append historical `build`, `.local-boost171`, or NDN-SVS build directories when
`NDNSF_LIBRARY_DIR` is set. The focused red/green regression is
`tests/python/test_python_wrapper_native_closure.py`.

## Resolved closure

The rebuilt extension has exactly this RUNPATH:

```text
/home/tianxing/NDN/ndn-service-framework/build-system-j2
```

`ldd` resolved the extension's framework library to
`build-system-j2/libndn-service-framework.so.0.1.0`. NFD, the extension, the
framework, NDN-SVS, and NAC-ABE all resolved `libndn-cxx.so.0.9.0` to:

```text
/usr/local/lib/libndn-cxx.so.0.9.0
SHA-256 cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531
Build ID 189995208664f28f4001cbe08c668d5e2b48937e
```

Recorded artifact identities:

| Artifact | SHA-256 |
|---|---|
| `/usr/local/bin/nfd` | `1d57645946bb096364d736db0d27a13a4e98c9abc4de62eb558762359a4d4a09` |
| `build-system-j2/libndn-service-framework.so` | `ef607f1569a916dd8747c0c4783856d597a598fc6281a07a68b539eb8501eae1` |
| `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so` | `9fe2bdc9bf5fe2f9f191dda1f11944a86a348ad2de118a95aa739e2c170844ee` |
| `/usr/local/lib/libnac-abe.so` | `1ca3f33ce934e84df5cf6ce2497c42a68999fba39a22f5f6383abb04f482cd05` |
| `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so` | `bf15706ececdbdaab635320c0723a2128b0a6705cf0be86fc6e2e01569643a4e` |

## Acceptance checks

```text
2 passed in 0.28s
NDNSF_IMPORT_PASS pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
NATIVE_LIBRARY_CLOSURE_PASS
```

No `LD_LIBRARY_PATH` override was used. This evidence closes S0 only; it does
not claim that the signed YOLO candidate, MiniNDN cases, SIF, or Tiger run has
passed.
