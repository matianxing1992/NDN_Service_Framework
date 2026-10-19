# Pinned OpenABE ownership and AES block-safety corrections

The host source lock binds this series to OpenABE commit
`cb2a18c2d2845a03ed502df4b77f596b55e48ebd` and checks both descriptor and patch
SHA256. The installer applies it to a private clean archive using
`scripts/stack_sources.py materialize`; it never edits the vendor checkout.

The ordered series contains two narrow patches. The first releases a live
object's final reference on the 1 -> 0 transition. The second configures EVP
padding after cipher initialization, checks that the private CTR-DRBG AES
helper produces exactly one block before copying, and enforces the previously
unasserted third NIST vector. Public signatures, object layout, and intended
policy/crypto semantics stay unchanged.

Original observed failure: Spec210 B8's unsuppressed ASan/UBSan/LSan run passed
17 native CP/KP/revocation cases (163 assertions), then reported 1024 bytes in
8 leaked policy leaves. Do not suppress this failure. The focused Waf target
`spec202-openabe-policy-lifecycle` checks literal destructor counts and
AND/OR/nested parsed/cloned structure against the actual installed library.
The pinned CTR-DRBG native target checks all 16 literal upstream NIST vectors.
The original unsuppressed three-repeat matrix, these native oracles, and
coherent installed-loader checks must pass after delivery; source review or
compilation alone is not runtime qualification.
