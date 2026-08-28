# T015 native Provider build boundary

**Date**: 2026-08-24  
**Status**: native target build verified; Python-to-native workload proof remains open

## Reproducible build subject

The initial `waf` invocation resolved `/usr/local/lib/libndn-svs.so`, an older
installation that did not provide the Experimental catch-up/statistics API
used by the current NDNSF source (`getMappingFetchStats`,
`getPublicationFetchStats`, `getPiggybackStats`, and
`subscribeToProducerWithCatchUp`). That failure is an environment/library
closure error, not a production fallback.

The target was then configured against the local NDN-SVS Experimental source
and build pair:

```text
./waf configure --with-examples \
  --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs \
  --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
./waf build --targets=di-native-provider -j2
```

The local NDN-SVS Experimental checkout is at commit
`6bb3454` (`svspubsub: bound segmented fetch and repair recovery`). The
resulting native executable is:

```text
build/examples/di-native-provider
sha256:20e5106d48cc3f785d734200db57eba061a198859c70a71facc2e2c4385a7884
```

`ldd` resolves `libndn-svs.so.0.1.0` to
`/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0` and resolves ndn-cxx
through the repository's Boost-1.71 prefix. The executable reaches its
argument parser without an unresolved-loader error.

This evidence proves only that the native Provider can be built against the
intended Experimental SVS API. It does not close G0/G1, SIF promotion, or the
fresh Python-user-to-native-Provider workload process required by T015/T020.
