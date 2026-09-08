# T004 native dependency cutpoint — 2026-09-07

Status: IMPLEMENTED / focused native transport checks passed. Negative User
terminal handling, distributed negative dispatch and retained collector remain
unwired. T004/T007/T015 remain open; no MiniNDN, SIF or GPU PASS is claimed.

## Boundary and ownership

The registered fault must withhold a real planned intermediate object after
Selection. An arbitrary killed process, pre-Selection route removal or generic
timeout cannot establish this condition.

`NdnsfCollaborationDependencyIo::OutputPublicationGate` is a generic optional
application-owned V3 control. It runs after configured dataflow authorization,
capability/endpoint ownership checks, tensor sealing and manifest encoding,
immediately before any signed exact Data publication. Empty means ordinary
publication; false withholds the complete object; exceptions remain execution
failures. No wire field enables it. Legacy/local output paths are unchanged.

`NativeProviderHandlerConfig` passes the callback to request-scoped IO. The native
executable accepts `--withhold-v3-output <request-id> <producer-role> <consumer-role>`.
It matches the real edge and first attempt, rejects shared-consumer objects for
this single-edge fault, and emits `NDNSF_DI_OUTPUT_WITHHELD` through the existing
runtime evidence logger. The structured record binds request/session/attempt/plan,
provider/boot identity, planned object/manifest names, endpoint/content digests,
payload size and time. No tensor bytes, keys, grants or Selection payloads appear.

Tiger's provider launcher enables this only for `DetectShard0` in the bound
one-request/non-warmup `negative-dependency` plan, targeting `Merge`. Other roles
and normal cases receive no fault argument. Public negative submission remains
blocked until the complete owner and collector are implemented.

## Retained verification

Root: `Experiments/TigerCluster/results/spec183-dependency-cutpoint-20260907/`.

| Evidence | Result | Scope |
| --- | --- | --- |
| `provider-argv.xml` / `.log` | 27 passed, 2.75 s | Provider arguments, negative role/request binding and rejection before launch; fake process boundary |
| `WithheldValidatedOutputPublishesNoManifestOrSegments.log` | 1 case / 12 assertions passed, 5.97 s | New production IO: one callback, zero manifest/segment Data, consumer throws `failed to fetch signed exact Data` |
| `PublicationGatePermitPreservesTensorBytes.log` | 1 case / 29 assertions passed | Permitted publication reconstructs original tensor bytes |
| `MalformedEndpointRejectsBeforePublicationGate.log` | 1 case / 4 assertions passed, 0.81 s | Invalid endpoint rejects before callback; zero callback invocations/packets |
| `syntax.json`, `syntax-*.log`, `syntax-executable-final.log` | Exit 0 | Current handler/executable syntax, including final evidence logger |

The C++ cases use the existing in-process integration environment with actual
capability sealing and exact signed Data verification. They are not cross-process
YOLO, model assembly or cluster qualification. The fixture supplies a V3 capability
but not a protected model grant; configured grant-before-hook ordering is source
evidence, not measured protected-runtime acceptance. The native CLI was syntax
checked; the full deployed binary has not yet run with this flag.

The scoped test executable recompiles tests/main.cpp, the integration fixture,
the exact-tensor test and changed IO into the result directory, with at most two
compilers. Unchanged DI objects are reused through a private archive; changed
IO/handler/runtime objects are excluded. `cached-objects.json` records size/mtime
stability and archive digest; `link.map` records selected members. Nothing was
written into build-system-j2. `compile*.json`, `link-*-command.json`, `ldd.txt`,
`linked-library.json`, `ndn-cxx-identity.json` and `native-tests.json` retain commands
and identities. NDNSF/NAC-ABE/NDN-SVS resolved from `/tmp/t008-build-root/lib`;
ndn-cxx resolved from `/usr/local/lib/libndn-cxx.so.0.9.0`. Recorded core and ndn-cxx
hashes remained unchanged across execution. This is not candidate promotion.

Initial command failures remain in logs: omitted NAC-ABE macro/include paths,
missing separately linked DI objects, an unnecessary unavailable zstd link flag,
and a wrong assumption that ndn-cxx resided in the clean prefix. Corrected commands
follow observed build wiring/loader closure. Successful main compilation was
reused; only the three new native cases ran. Another client's live waf PID1580053
was left alone. The syntax follow-up used one compiler in this independent output.
No historical native suite, SIF build, model or GPU test was repeated.

## Remaining path

Bind the emitted cutpoint plus consumer failure to retained User Selection and
dependency evidence. Add one bounded negative User terminal path, zero success
response/reselection and complete cleanup evidence. Route the same two ranks
through it and join evidence after srun before removing NEGATIVE_RUNNER_NOT_WIRED.
Then re-audit T007, reseal changed native/worker source and final harness, rebuild
affected native consumers, and obtain actual qualification. Old source/native
receipts cannot qualify this changed candidate. T015 still requires T014 first.
