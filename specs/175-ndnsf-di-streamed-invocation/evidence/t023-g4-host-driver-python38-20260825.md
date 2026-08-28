# T023 G4 host-driver compatibility failure

The first exact-SIF replay attempt stopped before MiniNDN topology creation.
The host replay driver was executed by the host Python 3.8 environment and
raised `AttributeError` because it called Python 3.9's
`str.removeprefix("sha256:")` while comparing the SIF digest. This was a
host-driver compatibility defect, not an SIF, ONNX Runtime, NDN, or NDNSF
runtime failure.

The failed attempt is retained as negative evidence. The driver now uses an
explicit prefix helper compatible with Python 3.8 and defaults to the
canonical workload seed `1750001`; a focused regression rejects the removed
API. Because the driver is embedded in the candidate SIF, the next candidate
must receive a new source seal, local build, runtime preflight, and exact-SIF
replay. No host runtime fallback is permitted.
