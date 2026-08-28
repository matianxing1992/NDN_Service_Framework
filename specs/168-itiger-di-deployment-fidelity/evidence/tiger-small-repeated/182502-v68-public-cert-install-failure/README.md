# Job 182502: rejected peer-certificate PIB installation

Job 182502 is the single formal TigerCluster campaign for immutable Spec 168
candidate v68 (`sha256:5f0ef716293aa9bfbd5a579d6840f9c2a6ab155ec5c78aa84489db4ad4262747`).
It terminated before the User request gate, model transfer, or inference.

All three Providers started and exported their public certificates.  The launch
script then attempted `ndnsec cert-install -N -f` for peer certificates.  Ranks
1 and 2 reported that the rank-0 Provider identity did not exist; rank 0
reported that the rank-1 Provider key did not exist in its PIB.  This is the
expected rejection of a semantically invalid operation: an ndn-cxx PIB stores
local identity/key state and is not a general peer-certificate cache.

The replacement contract keeps private identities isolated.  Each runtime's
existing `CertificatePublisher` serves its public certificate under its NDN
KEY prefix; every rank must fetch the exact expected certificate bytes over
NDN before the request gate opens.  `MessageValidator` then authenticates
runtime Data through its configured network certificate fetcher.  A local PIB
lookup miss is therefore diagnostic, not terminal; the formal analyzer blocks
on final validation failure and requires successful configured-validator
evidence.

This campaign is closed and must not be retried under the same identity.  The
retained result has `terminal-state=FAIL`, batch exit code 1, and one scrubbed
secret file.
