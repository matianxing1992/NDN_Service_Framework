# Spec170 Tiger storage cleanup audit (2026-08-14)

Before SIF materialization, the project quota was exhausted. After explicit
confirmation, exactly the following 18 superseded Spec162/164/166/167/168
directories were removed from `/project/tma1/ndnsf-di`:

```text
images/spec162-6216ef2b952
images/spec162-eaf7c064c0fc
images/spec162-fix016-10a52db40a1e
images/spec164-b94403035ed7
images/spec164-c296d2c371d3
images/spec164-dd8a80f95f8b
images/spec164-f150978cc004
images/spec164-sdk-9b712669f00f
images/spec168-v50-280c7dcb2184
releases/spec162-t009-fix019-core-bindings
releases/spec162-t009-fix020-core-bindings-abi
releases/spec162-t009-fix024-core-bindings-abi
releases/spec162-t009-fix025-core-bindings-abi
releases/spec162-t009-sif-scopekey-20260802T071000Z-001
releases/spec166-dcef2858c060
releases/spec167-native-file-producer-0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92
releases/spec168-v50-280c7dcb2184
releases/spec168-v51-193076fbed7e
```

The exact pre-delete total was `76414050304` bytes; the post-delete audit
reported zero remaining matches and a successful one-byte write probe. The
current project tree, including the accepted Spec170 release, remained intact.
Deletion was from project storage (no trash/undo), so the listed superseded
artifacts are not recoverable from that filesystem.

The materialization job subsequently used node-local `/scratch`/`/tmp` on
`itiger05` (about 14 TB available) for the 4.4 GB SIF build, then atomically
promoted and rehashed the project copy. This is the preferred use of scratch:
temporary build workspace only, with the verified SIF and manifest retained in
the project release directory.
