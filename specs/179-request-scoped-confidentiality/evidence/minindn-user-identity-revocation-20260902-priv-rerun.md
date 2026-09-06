# Spec179 MiniNDN user-identity revocation rerun

Date: 2026-09-02
Scenario: `user-identity-revocation`
Build: `build-clang-nodbg` (current local tree)
Command: `sudo -n timeout 180s python3 tests/minindn/run_request_scoped_confidentiality.py --execute --scenario user-identity-revocation --lifetime-ms 12000`

The privileged seven-node MiniNDN campaign completed successfully:

```text
status=completed
gatePassed=true
networkEvidence=true
revocationApplied=true
controllerVersion.generation=1788389368714
controllerVersion.epoch=2
requestPublicationCount=156
selectionPublicationCount=76
responsePublicationCount=31
executionCount=16
refreshAttempts=17
terminalOwner=/example/hello/provider/B
terminalReason=response_callback
redactedTraceHash=sha256:459d5a512c8ec95ce50e21b1a8f2a724e97eb2f16b336a2066624624afcc145a
processReturnCodes={controller-1:0,provider-A:0,provider-B:0,user-A:0,user-B:0}
elapsedSeconds=33.633
```

This is real cross-process normal-mode evidence for User identity withdrawal
with the matched two-User/two-Provider topology. It does not claim the
service-scoped, certificate-only, large-response, Targeted, stream, restart,
offline/rejoin, or source-equivalence scenarios; those remain separate
MiniNDN rows in the validation matrix.
