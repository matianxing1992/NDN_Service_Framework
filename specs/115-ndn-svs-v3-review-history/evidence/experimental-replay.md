# Final Experimental Replay and Ownership Evidence

Local `Experimental` is now:

```text
8335643f81be8fe3d49cf6e773569a21762049c9
tree fc5743665da77667930850969a1c05489733c8f4
```

It was constructed from `master@e42996d` plus seven separately owned commits:
the Boost baseline, sparse Mapping, duplicate fetch suppression, bounded
segmented recovery, transactional publication, generic extension transport,
and bounded Mapping/Repair policy. After validation, local master was explicitly
fast-forwarded to the same `8335643f` head.
The complete V3 owner `eb0754d` is an ancestor of both active branches.

The two former external-harness commits (`9f007ac`, `7fd50f9`) were removed from
the active Experimental line. A repository tree scan confirms neither master
nor Experimental tracks `tests/interop`, NDNts package locks, Node dependencies,
or cross-implementation peer programs. The NDN-SVS working tree is clean.

The old interop head remains recoverable at:

```text
safety/spec115-ndnsf-interop-ownership-20260716T2310Z
  -> 7fd50f9951de75fc04d67a7c036f86bb5ec9a941
```

External interoperability assets now belong to NDNSF at
`examples/interop/ndn-svs-v3/`. The final exact OID `8335643f` passed 71/71 ASan
units. The earlier, tree-identical `70e682f` ran the NDNSF TypeScript/C++
standalone matrix at 5/5 and the NDNSF-owned MiniNDN matrix at 6/6; those network
results remain explicitly tree-bound rather than claimed as a rerun.
