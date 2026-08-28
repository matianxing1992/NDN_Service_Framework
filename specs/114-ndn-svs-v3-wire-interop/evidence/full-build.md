# Full build evidence

## Spec 115 rewritten-history candidate

The rewritten Experimental candidate `fd99b3f51bdd048c783a023f1082a7091a27bac9`
has the same tree `be3d9ccd348377160ab7afffabcbdea9459cff4f` as the
previous validated `53dd158` candidate. A fresh configure/build passed, followed
by 67/67 unit cases and 398/398 assertions. The replacement V3 owner
`218ad952d8b5527bea9d93df6873f4afb995406c` also passed its exact-commit build,
41/41 cases, and 230/230 assertions using external harness inputs.

Date: 2026-07-16

## Final NDN-SVS identity

- commit: `53dd1588201b967a4aa9decd3e51ade3263e0f88`
- tree: `be3d9ccd348377160ab7afffabcbdea9459cff4f`
- branch/worktree: `Experimental`, clean
- compiler: `g++ 9.4.0`; ndn-cxx: `0.9.0`; Boost: `1.71`
- workspace and installed `libndn-svs.so` SHA-256:
  `9988a7e35523cd86dd5feb253078e839a4f8895f7693aeada2e7b3475a70ab92`

`./waf build -j$(nproc)` succeeded and `build/unit-tests` reported 67 cases
with `*** No errors detected`. The C++ interop peer and examples also built.
Workspace/installed `core.hpp`, `svspubsub.hpp`, and `sync-protocol.hpp` hashes
match exactly.

## Rebuilt NDNSF consumers

`./waf -j$(nproc)` rebuilt all 334 configured targets against the installed
candidate in 2m8.349s.

| Artifact | SHA-256 |
|---|---|
| `build/libndn-service-framework.so` | `bde4b203ebea0b3b074fdbcb25f9245138d73a0582cf51d1c23ce214c8debd5c` |
| `build/unit-tests` | `f78a7cd372b4695185113ee5d1065347d838717b3e97e7bcef61f4a21a053c90` |
| Python `_ndnsf` extension | `c065fa49a38e206420bcbf4b3025aff60be12270c911800489a942e9e09c2c15` |

No Docker, iTiger, merge, push, or master-ref action was performed.
