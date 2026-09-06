# Specification Quality Checklist: Request-Scoped Confidentiality and Epoch Revocation

**Purpose**: Validate that Spec179 is complete, testable, and claim-bounded.

## Content Quality

- [x] User value and security boundaries are explicit.
- [x] ABE discovery, request keys, response keys, and revocation are separated.
- [x] Historical-key/non-retractability limitation is explicit.
- [x] Scope excludes replacement of NAC-ABE or Provider-selection semantics.

## Requirement Completeness

- [x] No unresolved clarification markers remain.
- [x] Requirements are testable and unambiguous.
- [x] Success criteria cover positive, negative, replay, and revocation cases.
- [x] Normal, large, streaming, restart, and mixed-version edges are listed.
- [x] User identity, Provider identity, certificate-only, User
  `/PERMISSION/<service>`, and Provider `/SERVICE/<service>` revocation are
  distinguished, including a dual-role identity.
- [x] The current NAC-ABE limitation is explicit: every authorization-reducing
  change creates a fresh global public-parameter/master-secret generation and
  reissues filtered DKEYs to all retained identities.
- [x] Grant-only updates are distinguished from withdrawals: they reuse the
  controller-private master key and current public-parameter generation, and
  issue one replacement full-attribute DKEY only to the granted identity,
  without transmitting or invalidating unaffected DKEYs.
- [x] All six invocation cut points, cache families, offline/restart, Controller-unavailable, and reauthorization cases are inventoried.
- [x] Revocation convergence is bounded by scheduled pre-expiry refresh even when no peer carries a newer version.
- [x] Data entities, contracts, assumptions, and rollback are defined.

## Security Readiness

- [x] Recipient-specific key delivery is required.
- [x] AAD, certificate digests, both ControllerVersion fields, selection digest, and nonce policy are fixed.
- [x] Telemetry is prohibited from logging plaintext or key material.
- [x] Revocation is forward-looking and cannot overclaim historical erasure.
- [x] Every revocation requirement and security-critical state transition maps to deterministic unit and real component integration coverage.
- [x] A matched unaffected identity/service control is required for every revocation scope test.
- [ ] A retained pre-revocation DKEY is proven unable to decrypt new-generation
  ciphertext, while a retained identity's new filtered DKEY succeeds and mixed
  public-parameter/DKEY generations fail closed.

## Notes

Implementation must not mark the confidentiality/revocation claims complete
until every RV-U/RV-I row in `validation-matrix.md`, cross-user decryption,
obsolete ControllerVersion, repeated restart, clock rollback, concurrent-writer,
affected/unaffected control, and reauthorization tests pass.

## Controller revocation test sufficiency

- [x] Current-source focused evidence is recorded separately by layer:
  Controller-policy unit 31/31, refresh-coordinator unit 11/11, Controller
  component integration 34/34, refresh component 1/1, request-scoped
  selection 3/3, stream regression 19/19, and MiniNDN contract 3/3.
- [x] Unit cases cover version ordering, typed target validation, persistence/
  fencing, scope decisions, cache invalidation, expiry, and forward-only
  reauthorization.
- [x] Component cases cover real Controller mutation/status construction,
  timestamped recipient-bound permission snapshots, malformed/status-signature
  negatives, and the currently wired LocalMock User/Provider boundaries.
- [x] Component runtime case proves that no normal Request publication or
  Provider execution is allowed before an authenticated status is installed;
  the cross-process/trust-schema version remains a separate gate.
- [ ] Live User/Provider cases prove that a message-carried newer version
  causes exact signed-status retrieval rather than local version adoption;
  the coordinator unit test covers the non-adoption rule only.
- [ ] Live signed permission renewal denial, timestamp monotonicity across
  Controller restart, and the full target × cut-point × mode × recovery matrix
  pass through the configured trust schema and MiniNDN network gate.
- [ ] Live Controller withdrawal rotates the global NAC-ABE generation,
  publishes generation-bound public parameters, and reissues filtered DKEYs to all
  retained identities without issuing usable new material to the withdrawn
  identity/attribute.
