# Spec180 iteration-52 source correction evidence

Date: 2026-09-03

The native `AckAuthenticationEvidence` projection now carries an explicit
`trustSchemaValidated` marker. It is set only when `ServiceUser` receives an
ACK through the packet-backed validated subscription path, and is preserved by
the pybind `AckCandidate` and Python facade. `ProviderOfferTrustVerifier`
requires this marker before invoking its configured Trust-Schema callback.

The maintained YOLO example no longer verifies Provider offers with the
historical caller HMAC map. It loads the candidate-bound
`spec180-provider-offer-trust-v1` policy and a signer-key-ID-to-PEM map, then
delegates ACK packet-authentication authority to the native provenance marker.
The deprecated HMAC option is rejected by the maintained path.

Validation after the correction used one core-library build
(`./waf build --targets=ndn-service-framework -j2`, 20/20, 5m36.927s), one
Python 3.8 pybind build (`python3 setup.py build_ext --inplace --force`),
native import inspection (`AckCandidate.trust_schema_validated` is present),
and `ldd` closure against the repository's Boost 1.71, ndn-cxx, ndn-svs, and
NDNSF libraries. The focused Spec180/Spec175 compatibility collection passed
113 tests with 19 existing exporter warnings.

This is a focused source correction, not qualification evidence. A real
certificate-chain callback and the Y-A/Y-B/Y-N ACK-to-Response runner are still
missing; T006, T009, T011, T014, and all downstream local/SIF/Tiger tasks stay
open until those paths execute and pass the convergence gate.
