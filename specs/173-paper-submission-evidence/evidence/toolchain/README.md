# Spec 173 frozen C++ toolchain

This directory freezes the tracked dirty state used by the Spec 173 campaign.
Apply each gzip-compressed binary patch to the exact `HEAD` named in
`../toolchain-manifest.json`; no untracked C++ source file participates in the
NDNSF build graph.

Two initially plausible builds were rejected before measurement. Selecting the
Linuxbrew Binutils 2.47 linker against the Ubuntu GLib/GStreamer stack produced
unresolved transitive symbols. After switching to `/usr/bin/ld` 2.34, the first
successful link still loaded `/usr/local/lib/libndn-service-framework` and its
older NDN-SVS because `/usr/local` preceded the example's `$ORIGIN` RUNPATH.
The frozen build uses the system linker, the build-tree framework, the local
Boost-1.71 ndn-cxx/NAC-ABE libraries, and the isolated Experimental NDN-SVS.

The first complete NDNSF unit run exposed a flaky streaming test snapshot. The
test's two face event loops were observed after roughly 120 ms, at the exact
Mapping Interest expiry boundary. A final Consumer slice could also leave the
corresponding Provider callback queued. The regression now observes after
roughly 40 ms and polls the Provider queue without advancing time; it retains
the original pending-window, retry-count, lifetime, and delivery assertions.
The corrected test passed 200 consecutive focused repetitions followed by the
complete 464-case unit suite.

Accepted verification:

- NDN-SVS unit tests: 77/77.
- focused Mapping prefetch race: 200/200.
- NDNSF unit tests: 464/464.
- NDNSF integration tests: 11/11.

The first NDN-SVS invocation from the NDNSF root is not counted: six cases could
not locate relative `.hex` fixtures. Re-running the same binary from the
NDN-SVS source root found the fixtures and passed all 77 cases.
