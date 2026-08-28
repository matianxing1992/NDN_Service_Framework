# Spec 112 Pre-Fix Code Reality

**Captured**: 2026-07-15 CDT

**Evidence level**: source-verified and environment-executed. No MiniNDN defect
cell has run yet, so none of the five reported defects is marked reproduced by
this file alone.

## Reporter Lineage

The external report describes:

- NDN_Service_Framework Quarmire fork at `d2313d6`;
- ndn-svs matianxing1992 fork at `0521665`;
- ndn-cxx 0.9.0, NAC-ABE/OpenABE, Python `ndnsf`;
- aarch64/NixOS and one-hop Wi-Fi;
- an optional workaround patch at Quarmire commit `1485b07`.

Those revisions are diagnostic lineage only. They are not cherry-pick sources
for the current candidate.

## Current Source Identities

| Repository | Branch | HEAD | Tracked binary-diff SHA-256 | Porcelain-status SHA-256 |
|---|---|---|---|---|
| `ndn-service-framework` | `Experimental` | `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6` | `a4bd4cc1965d4b18f99001fff0b4f98466e33ea7bd80a383faa73ca4e1944346` | `d99e67f3025f31e4b2c5a9b4fc32e02809fcee4ef4c8f811843822c056b255e2` |
| `../ndn-svs` | `Experimental` | `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` | `8db8f198cbd80a39faab273349900415e07b434ce113a11a365aac21d75b0237` | `cf79a0fd9c00014bdd703c64d23f34a3c2210d12aff1e6339616dbd379fc044f` |
| `../NAC-ABE` | `master` | `1cc17d9d21f4dfc0921cc77315d0c57d46291880` | `00af56e276051cdcb41d947c18f3b36d308e253d20003b35c1e1e6d1dc3867f2` | `b0d4adf5533cd681083f38af1114823e40c54815a2239c9cbc98b3b18c912a9f` |

All three worktrees were dirty before Spec 112 implementation. In particular,
the exact ndn-svs and NAC-ABE target files already contain uncommitted changes.
Spec 112 must preserve them and record every subsequent candidate from complete
tracked diffs plus relevant untracked input contents; the hashes above are an
initial snapshot, not the final candidate ID.

## Installed And Existing Binary Identities

| Artifact | SHA-256 | Size | Modification time |
|---|---|---:|---|
| `/usr/local/lib/libndn-svs.so.0.1.0` | `773cb056b7355f7a4225ffb7aed848285eccadeabb92bea3fcf817f8eab6f7a5` | 4,862,328 | 2026-06-28 06:34:44 CDT |
| `/usr/local/lib/libndn-cxx.so.0.9.0` | `cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531` | 47,995,248 | 2026-05-28 17:04:49 CDT |
| `/usr/local/lib/libnac-abe.so` | `1ca3f33ce934e84df5cf6ce2497c42a68999fba39a22f5f6383abb04f482cd05` | 4,869,248 | 2026-07-10 07:07:44 CDT |
| `/usr/local/lib/libopenabe.so` | `69a37331b240ffa91b3886cbd2747f72593c7ac3976a0db6456629222c731f10` | 18,487,976 | 2024-01-07 15:25:25 CST |
| `../ndn-svs/build/unit-tests` | `221bc508a94065cd4ba8ab5baa3f1a19040a9e6b09afafd6ec70e84e32bc56f3` | 2,128,072 | 2026-06-01 16:57:23 CDT |
| `build/unit-tests` | `a93bac3b69b552101928dff6f4f3a5acd83f77e0c21df15c1ad544562f868792` | 240,504,632 | 2026-07-15 02:01:39 CDT |

The existing ndn-svs unit-test binary predates the current dirty source by more
than six weeks and is inadmissible until T008 rebuilds it.

## Toolchain And Capacity

- OS: Ubuntu 20.04.3 LTS, Linux 5.15.0-139-generic x86_64.
- GCC/G++: 9.4.0.
- Python: 3.8.10.
- ndn-cxx: 0.9.0.
- Boost development package: 1.71.0.
- Root filesystem at capture: 189,110,804,480 B total,
  21,073,997,824 B available (89% used).
- Passwordless sudo: available.
- Live MiniNDN/Controller/Provider/User owner: none; the only `pgrep` match was
  the preflight shell's own command line.

## Current Code Facts

1. `../ndn-svs/ndn-svs/svspubsub.hpp` retains
   `MAX_DATA_SIZE = 8000`.
2. `SVSPubSub::publish` and asynchronous preparation sign an inner segment and
   place its wire encoding inside a separately signed outer sync Data packet.
3. ndn-cxx 0.9.0 declares `MAX_NDN_PACKET_SIZE = 8800`.
4. asynchronous commit state can advance over a prepared failure, and several
   `Face::put`/validation callback boundaries lack complete failure handling.
5. NDNSF's existing `NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE` flag makes
   the response threshold zero and leaves the response inline; Spec 112 uses it
   only to reach the reported SVS path.
6. the current Python binding already enables tokens on Provider and User and
   registers services as `NormalAndTargeted`; this is implemented but not yet
   executed as Spec 112 evidence.
7. `ServiceUser::publishAdmittedPendingCall` calls `PublishRequestV2` before
   scheduling the request timeout.
8. current NAC-ABE uses a process-wide OpenABE executor and an empty
   `ABESupport` destructor; this is implemented but not yet lifecycle-proven.
9. `../ndn-svs/wscript` currently requires Boost 1.74 even though the declared
   project-local baseline is Boost 1.71, blocking a valid rebuild.

## Candidate Boundary

The first admissible pre-fix candidate will be created only after T007-T010 have
produced a buildable Boost 1.71 test configuration and finalized diagnostic
scripts. It will hash the complete relevant untracked inputs and rebuilt
binaries, not reuse the preliminary hashes in this file.
