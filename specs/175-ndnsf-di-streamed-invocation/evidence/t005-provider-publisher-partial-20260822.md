# T005 Provider Publisher Partial Evidence

Date: 2026-08-22  
Branch: `Experimental`  
Status: `PARTIAL`

## Implemented slice

`StreamEventPublisher` now provides a generic provider-side core primitive and
is exercised by the real one-Provider Normal integration path:

- queue admission precedes cursor/transcript commit;
- the event plaintext is bounded before publication;
- AES-256-GCM uses the committed event key and deterministic event nonce/AAD;
- the resulting Data is ECDSA-signed through the supplied `KeyChain`/`SigningInfo`;
- retained exact-name Data stores the immutable signed wire;
- `satisfy()` rejects prefix Interests and returns only an unexpired exact match;
- `republish()` returns and emits the same wire bytes rather than re-encrypting;
- End/Response completion fields share the terminal lifecycle owner.

## Evidence

```text
./waf build --target=unit-tests -j2
  PASS; 99/99

./build/unit-tests --run_test=Spec175InvocationStreamLifecycle \
  --log_level=message --report_level=no
  PASS; 10 test cases
```

`StreamEventPublisherEncryptsSignsRetainsAndRepublishesExactWire` verifies
ECDSA signature validity, AES-GCM recovery, exact-name satisfaction, identical
retry wire, bounded queue high-water, End completion, and terminal duplicate
rejection.

## Remaining T005 work

The real integration fixture now proves SVS publication, exact Interest
satisfaction, final Response closure, and Provider failure publication for a
selected Normal request. This does **not** close T005: loss/retry, queue
capacity/deadline, and broader fault/security cases remain open and continue
to block G1.
