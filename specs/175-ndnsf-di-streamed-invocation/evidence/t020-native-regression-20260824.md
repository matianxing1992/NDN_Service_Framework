# T020 native regression rerun

**Date**: 2026-08-24  
**Subject**: current Experimental worktree build, local Experimental NDN-SVS

## Executed

```text
./build/unit-tests --log_level=message
-> PASS, 567/567

./build/integration-tests --log_level=message
-> PASS, 67/67

binaries:
13a37b4ab5c7dfecbe05f2d6c0359319130bfa4ededf8a4ca1cb49e095a27493  build/unit-tests
a6a8dd638d467483c0758fc69503f51a4774eedb53126e930628e641c22799e6  build/integration-tests
20e5106d48cc3f785d734200db57eba061a198859c70a71facc2e2c4385a7884  build/examples/di-native-provider
```

Both test binaries resolve `libndn-svs.so.0.1.0` to
`/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`, and resolve ndn-cxx
through the repository Boost-1.71 prefix. The native integration output
includes I01-I15, including I12 replacement, I13 live transport detach, and
I15 ACK-driven role-map permutation.

The standard build intentionally reports skips for optional external ONNX
model paths when `NDNSF_DI_TEST_ONNX_*` variables are unset. Those skips are
not a Qwen3.6/Tiger qualification result. G0 remains blocked by the dirty
shared worktree, and the automatic Python-user-to-native-Provider process
proof, SIF, MiniNDN, CUDA, and Tiger gates remain open.
