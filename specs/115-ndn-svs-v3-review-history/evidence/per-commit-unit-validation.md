# Final Per-Commit Validation Binding

> Historical binding for superseded commit identities. See
> `boost174-compiletmp-validation.md` for the final 11 exact OIDs.

Captured: `2026-07-16`

## Method

The master ownership-fold commits retain their prior exact-tree validation.
Phase 10 independently checked out, configured, built, and ran the complete
unit suite at every final Experimental OID after its message and parent chain
were finalized. The exact Experimental head also passed the complete
AddressSanitizer suite.

## Master mapping

| Validated OID | Final OID | Identical tree | Unit gate |
|---|---|---|---:|
| `502b9e2` | `7804202` | `eb3b3cdec62df4b03a7e514ceed9206d297d6b72` | 8/8 PASS |
| `49ec007` | `eb0754d` | `db1a5e34675da2e54223d992cc0c3e36f5cc1d1c` | 44/44 PASS |
| `64d4b30` | `f8ef18f` | `94ff7ae00cc4c71ccb37d083e6895c2a00d56119` | 47/47 PASS |
| `c62f4b5` | `e42996d` | `d26e70e124cdf18efb5ba2ca964894bd79dfd7fb` | 47/47 PASS |

## Final Experimental sequence

| Exact OID | Tree | Unit gate |
|---|---|---:|
| `2a334c7a` | `d9c239e2e768571a0206d24b8ce78b1c73fe6925` | 47/47 PASS |
| `cce63dca` | `aa932c399cd8e0f795dfced15ad22124f2e7c3de` | 48/48 PASS |
| `e4687cb8` | `4897b317ea78041548b0ba7e1fbd71f9f8ad6a5b` | 49/49 PASS |
| `3ed1bc22` | `7dde2152b400626f7473f4b3bbf82832bd131ba0` | 60/60 PASS |
| `6939790d` | `c46d673c1c6121b3d32f7c1d4dace0e527a5bf2d` | 66/66 PASS |
| `751ab209` | `e73b54e0b0d9c7dcb195b302118bd2ebba7d3575` | 67/67 PASS |
| `8335643f` | `fc5743665da77667930850969a1c05489733c8f4` | 71/71 PASS; ASan 71/71 PASS |

The former harness commits `9f007ac` and `7fd50f9` remain reachable only through
a safety ref; they are not part of active NDN-SVS master or Experimental. Final
`Experimental@8335643f` was rebuilt from a fresh `distclean` configuration under
AddressSanitizer and passed the complete 71/71 suite.
