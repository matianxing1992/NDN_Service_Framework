# T008 / FR-008 protected-grant implementation gap — 2026-09-05

Status: **BLOCK**, not a failed model inference and not a request to redesign.
The existing protected-artifact contract remains authoritative. No grant issuer
was created, no secret was copied, and no plaintext exemption was introduced.

## What is already specified

`../contracts/ack-driven-yolo-v1.md` requires protected canonical artifacts.
The inherited Spec170 `contracts/artifact-assembly-v1.md` defines the ordering:
seal plan core, authenticate a grant request to the configured policy authority,
obtain Provider-recipient-encrypted signed grant Data, finalize the plan with
grant references, then verify and unwrap inside the selected Provider. Grant
requests bind the core digest, not the later final plan digest.

## Current implementation, not inferred from class names

- `core/protected_artifacts.py` uses a Python `repr` request digest and a
  pipe-joined HMAC signing representation. It does not define an interoperable
  signed NDN Data or recipient-envelope encoding.
- `security/artifact_policy_authority.py` is an HMAC helper accepting supplied
  wrapped-key bytes. It is not the authenticated, policy-checking, publishing
  authority required by the contract.
- `app_sdk/placement.py` has a grant callback seam, but its default role specs
  use `plaintext-v1`. The view passed to that seam lacks several authority,
  recipient-certificate, model, epoch-policy and revocation inputs required
  by the inherited grant contract.
- Native `ProtectedRuntime::verifyGrant` checks supplied bindings and expiry;
  copying Selection fields into it does not authenticate, fetch or unwrap a
  grant. The production assembler does not consume a verified key lease.
- `contracts/trust-root-registry-v1.json` configures catalogue and model-manifest
  signers, not an ArtifactPolicyAuthority. Neither existing signer is implicitly
  authorized to issue content-key grants.

Consequently, adding a callback/factory or changing a profile label alone cannot
satisfy FR-008. A self-generated epoch error cannot qualify Y-N-E.

## Required closure, within the accepted architecture

1. Bind an operator-authorized grant issuer identity/endpoint, trust rule,
   Provider recipient certificates, model access policy, content-key owner and
   revocation source. This authority assignment requires explicit operator input.
2. Complete canonical cross-language request/grant/revocation encodings and
   signature coverage, recipient envelope/AAD and digest definitions. Define
   the grant digest without hashing a field that contains that same digest;
   preserve the existing non-circular core/final-plan ordering.
3. Wire authenticated request, policy check, grant publication and complete
   grant-cover verification before Selection. Reuse existing cryptographic
   primitives; do not use the Repo fetcher's `ValidatorNull` as grant validation.
4. Wire Native exact-name fetch, expected-authority verification, recipient
   unwrap, protected artifact decryption, runtime key leases, expiry/revocation
   checks and zeroization. Reject plaintext for the registered protected case.
5. Add cross-language vectors and focused wrong-signer/recipient/request/attempt/
   core/model/epoch/expiry/revocation tests, then a real Y-N-E mutation reaching
   the Native verifier. Only after code-aware T014 PASS may T015 and later
   exact-SIF/Tiger qualification proceed.

This is the remaining production gap, not an ONNX Runtime versus GenAI choice.
The current inference technology route is unchanged. Earlier negative PASS
records are not reusable safety evidence; see
`t011-negative-verdict-repair-20260905.md`.
