# Spec 168 v58 Gate B - DistributedRepo runtime ABI mismatch

## Verdict

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `24069.467 ms`; no NDNSF
Request began and `automaticRetry=false`.

## Frozen identity

- Candidate: `20260804T085229Z-v58-overlay-first-abi`
- Source identity: `sha256:0b0d8102350f38502dbe21caba2ccd0a1d57d0159a9a7da72c7200f9fc0010f2`
- Source bundle: `sha256:5d4a3c6e543eef1aed61e614f67a55426a73de61d6ed54a0e11434a1a406faa9`
- Gate command: `sha256:827b08618072e3c89b3023958fd49d0fb119e7297baa95e351aaac8bf7a684b5`
- Gate manifest: `sha256:7e04abaf5d2153f34fe1940830bbe725dc7391be92ae5f4c764f5f3a7b4cf354`
- Gate checkpoint: `sha256:f1e42a4af1edbbe515a3058b4315286ef004f1bc57a53d21a2074407449a2294`
- Launcher log: `sha256:f8cc37718ba31d57b8bfad2af5662c0a239e335d9898fd930a289fc26f5d6cbe`
- Controller log: `sha256:5ac7e493155d7bc108675e144c1ce375a3fe04056cb1ab2c0d40024e6e56ea15`
- Repo 0 log: `sha256:7fba6daaa029ed5858b513959ac5ce28230e5a12c9cb459eabd11b54c4dbe80a`

## Narrowest boundary and cause

The overlay-first repair worked: entrypoint imports passed, Controller reached
ready, and Repo 0 obtained its certificate and entered `ServiceProvider`
construction. It then aborted with `double free or corruption (!prev)` inside
the NDNSF Python service constructor.

The image Repo extension
`sha256:3121dc524921d8a4e2a094f5ca05baaa084283549ec9aa1face0b9b36f579af4`
exports the required Python symbols but was compiled against the image's older
NDNSF core. v58 loaded the repaired current core
`sha256:92a52a655a525b1f180d3ccd8244a3aa4c43d7a128288c847f0b2f9bc3b6ac88`.
Import-only validation could not detect the C++ ABI mismatch; the first object
construction did.

## Repair boundary

No compatible existing Repo CPython extension was found. The replacement must
compile only that extension against the already-built current NDNSF core, then
pass an object construction/destruction smoke before another MiniNDN gate. The
foundation image, NDNSF core, Qwen artifacts, schedule, routes and Repo payload
remain unchanged. Cleanup removed the v58 container, NFD and NLSR processes.
