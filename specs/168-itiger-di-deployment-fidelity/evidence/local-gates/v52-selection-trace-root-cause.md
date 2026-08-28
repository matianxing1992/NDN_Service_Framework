# Spec 168 v52 trace diagnostic - stale native Selection parser

## Verdict

`BLOCK` — trace-only diagnostic, `EXEC_LOCAL_GATE_HARD_DEADLINE` after
`300003.980 ms`; `automaticRetry=false`. v52 was never eligible for Gate B or
TigerCluster admission.

## Frozen identity

- Candidate: `20260804T082041Z-v52-selection-trace`
- Source identity: `sha256:47c8b17c0a398b1049f591ea6f8662abfe11e8ad3c57048c0ddb931429d27224`
- Source bundle: `sha256:dd3da9e133a9dbd480aaca6b8407fdf009e87d7e71df3c819ad650d6f8552537`
- Gate command: `sha256:ec0089a5721b1c18b1cb23ffc304cc248f691fcf82943f8cf44e2d7f5e280f91`
- Gate manifest: `sha256:b9ba0b15ee51337bd0c49b55f8a741d7b5c994428422488eeae9bd77e147a6df`
- Gate checkpoint: `sha256:3a9aaf7b1d4967560e5c7d3143086ae4b1765ee9c75cabe96971fe303b2f47e5`
- Launcher log: `sha256:4f39e03bb36f22c46e4c04209157de01b5897dd32062f17d1bd6ab02b3bc9bac`

## Exact trace result

For Repo request `20260804T082402.166932-36edea37b51a7c15`, the selected Repo 0
issued an exact-name prefetch. The User direct-published and Repo 0 received the
same name and the same `2857`-byte Selection Data. Repo 1 and Repo 2 timed out
on their own provider-specific names because they were not selected. Thus the
failure is not NDN routing, name mismatch, missing direct-put, or payload loss.

Repo 0 validated the User-signed Selection and entered
`SELECTION_DECRYPT_START`, but produced neither success nor failure. The trace
then exposed the incorrect hybrid policy attribute:

```text
/SERVICE/%2Fexample%2Fllm-pipeline%2Frepo/NDNSF/DistributedRepo/Artifact/v2/STORE
```

The required service-level attribute is:

```text
/SERVICE/NDNSF/DistributedRepo/Artifact/v2/STORE
```

The installed native library in the v49 image classified the provider-bound
Selection as compact and included the encoded Provider component in
`serviceName`. NAC-ABE key unwrap could therefore never succeed. The current
repository parser already rejects this ambiguous classification, and
`V2RequestAndResponseNames` already verifies that a provider-bound Selection
cannot parse as compact. The passing host test and failing container are direct
evidence that the runtime native artifact was stale relative to the tested
source.

Retained runtime log digests:

- User: `sha256:d07192ffaf394624e8427f09a56b923fc02e8d56bbf2c39b88411762e422c969`
- Repo 0: `sha256:1f6375f53b478b7fa128def017fada519a21c909c1c7571fd6ec92d70be074fe`
- Repo 1: `sha256:1823df27b40e92b40c8bd41eb60dbeb6910237ea8b472d18064ce30ea47d26d2`
- Repo 2: `sha256:0f5cebd26a8a16beffe85645444675581cf9443da01d637f63b7d1f33a72d8c4`
- Stage 0: `sha256:13f9bd325747a4f6210c9a85f964f888e86f1ff6d74e6f798ff47f90a1609765`
- Stage 1: `sha256:54452bd4e573f0395aebd42817529ac498d1e26488fa9e23dbd1cafa337ee69b`
- Stage 2: `sha256:6ad42fcf23b8188125b08de1324b1ca9a9bb7e49129a533fd7824a576be12521`

## Repair boundary

No new protocol code or model preparation is required. v53 overlays the
already-built, already-regression-tested native ABI closure: core library
`sha256:92a52a655a525b1f180d3ccd8244a3aa4c43d7a128288c847f0b2f9bc3b6ac88`
plus its CPython 3.10 extension. The immutable bundle is
`sha256:fd920b757ab39c57703254d2c4438e21a9b7753507729ad306c6c4155acb8fd2`.
The foundation image, Qwen model, tokenizer, routes, schedule and Repo payload
remain unchanged.
