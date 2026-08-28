# Spec175 request-contract mutation audit — 2026-08-23

Status: `DEVELOPMENT_PASS`; this proves the native Provider rejects a Request
payload that does not match the accepted plan/projection digest. It is not a
sealed G1/G2 result.

## Subject

- Branch: `Experimental`
- Base commit: `8350cad1c5a0b013c56f9f79450b3eb3686b4e21`
- Rebuilt unit-test binary SHA-256:
  `13efd316868d0ab61bc5314d4a345d695b24b3c699968bd4a29fca1e3a4ee737`
- Rebuilt integration-test binary SHA-256:
  `9efb3230e2f6eb37e710189148a2f32ec5fec52d544137a91f5106bd74a91a9d`

## Exact-payload mutation regression

```bash
./build/unit-tests \
  --run_test='*NativeRequestContractDigestBindsExactPayload,*Spec175InvocationStreamMessage,*Spec175InvocationStreamLifecycle,*DiQwenGenerationSession' \
  --log_level=message
```

Result: `44` test cases, `*** No errors detected`.

`NativeRequestContractDigestBindsExactPayload` computes SHA-256 over the exact
raw Request payload and verifies all of the following:

- the canonical lower-case `sha256:` digest of the unchanged payload passes;
- changing one payload byte fails;
- a non-canonical upper-case digest fails;
- an empty digest fails.

The production native Provider calls the same helper before accepting its
assignment, so this is a direct parser/Provider mutation regression rather than
a duplicate test-only implementation.

## Healthy-path regression

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I02 \
  --healthy-repeats 1 \
  --seed 1750001 \
  --output /tmp/spec175-i02-after-request-digest.json
```

Result: `PASS`; manifest SHA-256
`f2f10b023a71c7935a6558ab2d0ffd74e32b6bc8db06036b1b2eab0abaaed5cb`.
This verifies that the stricter digest check does not reject the real two-role
healthy Request/ACK/plan/Selection/stream path.

## Boundary

The result does not close I12/I13, the real Python callback path, the full
fresh-process G1/G2 manifests, or any SIF/MiniNDN/Tiger gate.
