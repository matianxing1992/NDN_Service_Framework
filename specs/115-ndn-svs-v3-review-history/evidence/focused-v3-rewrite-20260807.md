# Focused SVS V3 Rewrite Evidence — 2026-08-07

## Result

The rewrite was performed only in the isolated sibling worktree
`/home/tianxing/NDN/ndn-svs-focused-v3-rewrite`. No planning, audit, result,
campaign, Node dependency, or external interoperability-harness file was added
to either NDN-SVS branch.

The local branch prepared for the focused pull request is:

```text
pr/svs-v3-focused
5db5e6a5ce4b5ad3e08988a76af96f6f1b96dcd3
tree d7fc7f34ddcf98ced4110c5084a30ae6eb29935a
```

Its exact `upstream/master..pr/svs-v3-focused` history is:

```text
de387c122007cc905ad672f55c3c9c5697150acb svspubsub: add regex subscriptions and name-only publishing
3584e6b794d1af2049621eff77d6cdff4e4513b8 security: encode V2 signed Sync Interests with InterestSigner
5db5e6a5ce4b5ad3e08988a76af96f6f1b96dcd3 svs: implement corrected version 3 protocol
```

The complete reorganized nine-commit history is now the local `master`:

```text
master
3c96ab431ade00cf43f1bc2d7528076c5dcd132f
tree 90fd61e6756ca983c5c3987f3bb249b5513b5fdf
```

Its exact six commits after the focused boundary are:

```text
1acac1d262083919a2b023e09ae9f0d9bbec0d3a svspubsub: piggyback bounded mappings and publication Data
5b0cb55f3a6631c0755997e062cae4912ca81fc9 sync: parallelize receive processing and batch local Sync Interests
b2cc11092a0f5b0abecd59ff78aa002411988cc0 sync: parallelize production and add ordered async PubSub publishing
69e1fec67d9def35207fa0d95697542eedc32a1e svspubsub: recover sparse mappings without duplicate fetches
7f49b9a7d9d6fde8da79ab1cdd22fd5974cb97d6 svspubsub: make segmented publication failure-atomic
3c96ab431ade00cf43f1bc2d7528076c5dcd132f svspubsub: bound segmented fetch and repair recovery
```

## Validation

The tracked NDN-SVS `wscript` remains at its original Boost 1.74 minimum. For
the local host only, exact-OID verification temporarily applied the canonical
build-only Boost 1.71 threshold patch and used this sanitized toolchain:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin
CC=/usr/bin/gcc
CXX=/usr/bin/g++
AR=/usr/bin/ar
```

Results:

| Candidate | Gate | Result |
|---|---|---|
| focused `5db5e6a` | clean configure/build, examples, complete units | PASS, 40/40 |
| focused `5db5e6a` | NDNSF-owned C++/TypeScript NDNts standalone matrix | PASS, 5/5; zero Sync-Ack |
| full local master `3c96ab4` | clean configure/build, examples, complete units | PASS, 74/74 |
| full local master `3c96ab4` | resolved Boost linkage | all Boost libraries 1.71; local build `libndn-svs` selected |

The current NDNSF interoperability peer sets three Fetcher/Repair options that
are intentionally absent from the focused branch. The focused interoperability
gate therefore used a temporary `/tmp` peer source with only those three
continuation-only assignments removed; all packet exchange, validation,
language, and case logic was unchanged. Its summary SHA-256 is:

```text
129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b
```

The final full-master build artifacts were:

```text
libndn-svs.so  e5dbb4b83138af1c43c52be87f57062a38f3b65e6ba38a9dd36538c3915a6698
unit-tests     31570da76fcede919c0d235182d601505f0a5cb7ce9965ba59cc94f4fc9f0e60
```

## Preservation and publication fence

The original dirty checkout `/home/tianxing/NDN/ndn-svs` remained byte-for-byte
unchanged throughout the rewrite:

```text
branch Experimental
HEAD 6bb34545b4f89f1f6c265a68c18f1a40ade413eb
dirty tracked paths 11
git diff --binary SHA-256 71ae22a1c3325ec84c65155ef1231be472a42be452a201968cb027281776bd98
git status --porcelain=v1 SHA-256 b6f5d90bc90c4a9f7c26166d2b6e2573b5b14b28da5208e7b13152a40abb1996
```

Remote identities after the rewrite are still:

```text
origin/master   6bb34545b4f89f1f6c265a68c18f1a40ade413eb
origin/pr/svs-v3-focused 5db5e6a5ce4b5ad3e08988a76af96f6f1b96dcd3
upstream/master a93724758aca71a4ea327574ef7af46770a81a40
```

After the user clarified the desired local layout, `master` was moved to the
complete reorganized head `3c96ab431ade00cf43f1bc2d7528076c5dcd132f`.
`pr/svs-v3-focused` remains at the three-commit boundary `5db5e6a`. The two
redundant `rewrite/*` alias branches were deleted, and `master` has no remote
tracking branch until the user decides how to update `origin/master`. The
focused branch now tracks `origin/pr/svs-v3-focused`. The old
local master remains recoverable through `safety/focused-v3-20260807-master`.
This local ref update did not change the dirty `Experimental` worktree or any
remote ref.

There are no local tag refs. On explicit user request, only
`pr/svs-v3-focused@5db5e6a` was pushed as a new origin branch. `origin/master`,
tags, and pull-request metadata were not changed, and no pull request was
created. The disposable assembly and focused-validation worktrees were removed;
no `compileTMP` branch remains.
