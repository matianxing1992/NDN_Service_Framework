# Spec 176 multi-segment named-evidence evidence

**Candidate**: `UAV-Experimental` working tree, 2026-08-28

## Command

```bash
./waf build -j2 --targets=integration-tests
./build/integration-tests \
  --run_test=UavCollaborationFlow/MultiSegmentNamedEvidenceUsesExactValidatedFetchForTwoConsumers \
  --log_level=message
```

## Result

The test returned code 0. It creates a three-Provider CPU fixture after the
normal bootstrap gate, replaces the default peer links with explicit in-process
DummyFace bridges, and uses a producer-owned, versioned UAV evidence name.
The producer publishes a 12,000-byte evidence object through
`CollaborationContext::publishLargeNamed()` with a 256-byte segment bound. The
observed object spans 48 signed Data segments. Two independent consumers call
`fetchLarge(..., expectedSegments=48)`; the fetch path issues exact Interests
(`CanBePrefix=false`) for every segment, validates every returned Data through
the configured `MessageValidator`, reassembles the encrypted object, and
recovers the original bytes for both consumers.

The first consumer then passes the recovered bytes through
`UavDetectorProvider::acceptValidatedContent()` and detector execution succeeds
only with the producer identity and declared SHA-256 digest. Existing UAV unit
and exact-Data tests separately reject endpoint-like names, wrong mission or
version lineage, wrong signer/tampered signatures, digest mismatch, invalid
retention, and duplicate/late evidence. The invocation payload remains a small
request marker; frame bytes and transport endpoint fields are not included.

## Boundary

This is a deterministic in-process multi-segment CPU/NDN integration result. It
does not claim multi-process MiniNDN, packet-loss/reordering resilience, or PX4
SITL deployment. Those remain separate promotion gates.
