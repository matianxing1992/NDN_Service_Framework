# NAC-ABE DKEY freshness for grant-only replacement

The current KP-ABE DKEY discovery Interest is unversioned and uses a
versioned, segmented Data response.  A DKEY response that remains fresh in an
NDN Content Store can therefore hide a newly replaced identity policy for the
freshness interval.  Spec179 requires the target-only grant refresh to obtain
the replacement complete DKEY, not a still-fresh copy of the old one.

`AttributeAuthority::generateDecryptionKeySegments()` now publishes DKEY
segments with `FreshnessPeriod = 0`.  Exact versioned segment names remain
retrievable; `MustBeFresh` discovery Interests cannot satisfy themselves from
an old fresh DKEY and are forwarded to the current Attribute Authority.  This
change is independent of the ABE master/public-parameter generation: it does
not rotate or expose the master secret.

The change is in the isolated NAC-ABE source checkout used by Spec179:

```text
/home/tianxing/NDN/NAC-ABE/src/attribute-authority.cpp
```

It must be rebuilt into `/tmp/nac-abe-spec179-exact-prefix` together with the
Consumer DKEY-only refresh-fence patch before any NDNSF binary is promoted.

The updated isolated build/install completed with `make -C
/tmp/nac-abe-spec179-exact-build nac-abe -j2` and `cmake --install
/tmp/nac-abe-spec179-exact-build`.  The resulting `libnac-abe.so` SHA-256 is
`7c972e5285b124ac8152a3d834c895dc7f2e1c2ffc0149ec826744a4276a6d91`.
The `attribute-authority.cpp` source diff SHA-256 is
`e3a055039c468ae3569747d8e4febb415f2b5db09948b30668e5057528a0294b`.
Required release verification remains an `ldd -r` closure for the NDNSF
candidate and a cross-process grant-only test that counts one target fetch,
zero unaffected fetches, and proves the new attribute is unavailable before
replacement installation.
