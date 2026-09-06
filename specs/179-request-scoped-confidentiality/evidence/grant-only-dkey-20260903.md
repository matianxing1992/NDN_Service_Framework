# Grant-only DKEY lifecycle evidence

## Contract

The current KP-ABE implementation stores one monolithic policy/DKEY per
identity. Therefore a grant-only authorization change advances
`ControllerVersion`, keeps the current ABE master secret and public-parameter
name/digest, replaces the complete policy for the granted identity, and makes
one complete replacement DKEY available on that identity's next lazy DKEY
fetch (normally initiated once after signed status installation, with explicit
fetch as fallback). The old same-generation target DKEY remains limited to its
previous attributes until replacement installation. It does not issue,
invalidate, or refetch DKEYs for unaffected identities. A transaction
containing any withdrawal uses the withdrawal path: fresh global master/public
parameters and filtered DKEY policies for retained identities.

## Focused evidence

Build inputs were the current NDNSF tree and the exact Spec179 NAC-ABE prefix
(`build-spec179-o0-exact`, Boost 1.71, `-O0 -g0`). The Controller integration
binary was compiled with Clang 10 because the host GCC 9 intermittently ICEs
while compiling this large test translation unit.

```text
/tmp/spec179-controller-policy-tests --run_test=ControllerRevocationPolicy
33 test cases, no errors

/tmp/spec179-controller-flow-tests-clang \
  --run_test=ControllerRevocationFlow/ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy
pass

/tmp/spec179-controller-flow-tests-clang \
  --run_test=ControllerRevocationFlow/RealRuntimeEnforcesCertificateAndServiceRevocation
pass
```

The grant-only component case observes a higher ControllerVersion, unchanged
ABE public-parameter name/digest, unchanged unaffected-provider policy, and a
complete `/PERMISSION/HELLO` policy for only the granted User. The current
component test inspects policy replacement; source compilation now also covers
the runtime pending-refresh hook, but it does not yet drive the real lazy DKEY
fetch and atomic runtime installation. It then grants
`/SERVICE/HELLO` to a Provider and observes the same unchanged pair. The
withdrawal that follows changes the ABE name/digest, confirming that the two
transactions do not share the same key-generation path. Reauthorizing that
withdrawn Provider keeps the new post-withdrawal pair rather than restoring the
pre-withdrawal generation.

The focused PolicyStatus case also rejects a missing name, missing digest, or
malformed digest, preventing a mixed generation identity from entering the
status wire format.

The complete Clang `ControllerRevocationFlow` run reached 37 cases; all other
cases passed. One unrelated live-refresh fixture still aborts in NAC-ABE's
`AttributeAuthority` signer because the isolated test KeyChain does not own the
certificate name captured by the AA. It is retained as a separate test-fixture
blocker rather than weakening the grant/revocation assertions.

## Remaining gate

This is component evidence, not the final cryptographic release claim. A
cross-process test must still fetch real DKEY Data before and after a global
withdrawal, prove that a retained old DKEY cannot decrypt new-generation
ciphertext, and count that a grant-only change causes exactly one target DKEY
refresh and zero unaffected refreshes. The full legacy integration binary also
contains a pre-existing live-fixture signer failure when an isolated memory
PIB is used; that failure is tracked separately and is not evidence against
the grant-only policy transition.

The updated grant-only test translation unit (including the retained-attribute
assertion) was rebuilt against the `build-spec179-grant-refresh` shared library
with the exact NAC-ABE headers (`-DHAVE_TESTS -DNAC_ABE_CMAKE_BUILD`, `/usr/bin`
linker) and passes:

```text
/tmp/spec179-controller-policy-tests-v2 --run_test=ControllerRevocationPolicy
Running 33 test cases...
*** No errors detected
```

This remains component evidence: it does not prove the cross-process lazy
DKEY fetch/installation count or retained-old-DKEY cryptographic exclusion.
