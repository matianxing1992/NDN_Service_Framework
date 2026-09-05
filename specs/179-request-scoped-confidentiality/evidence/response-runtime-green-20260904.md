# Request-scoped response runtime suites — executed 2026-09-04

Binary: `build-clang-spec179-nac3/integration-tests` (Clang 10, exact Spec179
NAC-ABE prefix `/tmp/nac-abe-spec179-exact-prefix`).

## RequestScopedResponseConfidentiality — 4/4 cases, no errors

```text
./build-clang-spec179-nac3/integration-tests \
  --run_test='RequestScopedResponseConfidentiality' --log_level=test_suite
Running 4 test cases...
*** No errors detected
```

Cases:

| Case | Coverage | Result |
|---|---|---|
| `LargeResponseUsesConfiguredTrustAndRequestBoundAead` | retained signed segment validated through the configured trust schema; request-bound AAD/nonce behavior | pass |
| `NormalResponseCompletesThroughProductionOnResponse` | full production Request→ACK→Selection→execution→Response path; request-scoped K_response decrypt through the real User OnResponse; compat counters zero | pass |
| `LargeResponseCompletesThroughProductionOnResponse` | production large response: 3 IMS-retained per-segment AeadEnvelope TLVs fetched with FinalBlockId termination, per-segment decrypt, digest check, reconstruction | pass |
| `RevokedUserCannotReceiveLargeResponseAfterProviderExecution` | Provider executes once with current-version material; User installs revoking identity status; arriving large response refused before delivery; exactly one terminal outcome | pass |

The production flow executes: User publishes request-scoped Request
(empty payload + capabilities) → hybrid-wrapped production ingress decrypt at
both Providers → ACK CK wrap under the re-armed NAC producer → automatic
Selection with K_input/K_response generation → selected Provider fetches the
exact User-signed Input Data and executes once → Response AEAD-encrypted with
K_response (inline) or per-segment (large) → User production OnResponse
decrypts with the Selection-envelope keys and delivers only the requesting
User. The old service-wide response-key carrier and its compatibility
counters were removed (Spec179 T012) after this suite and the MiniNDN campaign
passed; the removed counters therefore stay at zero structurally, and the
post-migration default is pinned by the unit regression
`RequestScopedDefaultActivationWithConfiguredController`.

## Supporting fixes (see restore-fixes-20260904.md)

- Fixture SVS publications now carry the role certificates (transport-owner
  check reads the KeyLocator).
- Fixture AA public-parameter name/digest accessors; status installs bind the
  real generation so the NAC re-arm completes.
- NAC-ABE `onPublicParamsRequest` serves the exact version-addressable
  params name without double-appending the suffix.
- Provider IMS discovery replies carry the final segment id.
- Resolved large responses skip the envelope decrypt at delivery (the
  per-segment AEAD + nonce registry + digest check already authenticated
  them).
