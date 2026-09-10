# R11-B9-G3 Cross-Process Dependency Boundary (2026-09-10)

## Result

`PARTIAL`: the first independent C++ requester/Provider conversation attempt stopped at
the Python extension import boundary before any protocol process was started. No
Controller, Authority, Provider, grant, selection, stream, or conversation result is
counted.

## First boundary

The driver returned `rc=1` while importing the current `_ndnsf` build:

`undefined symbol: ndn::nacabe::Consumer::clearCache(ndn::Name const&, std::string const&)`

The process environment placed `/usr/local/lib` before the matching NAC-ABE install.
The build's recorded RUNPATH expects
`/home/tianxing/NDN/nac-abe-integration-182/install/lib` and
`/home/tianxing/NDN/ndn-svs/build`; `/usr/local/lib/libnac-abe.so` does not export the
required symbol, while the matching installed library does.

## Raw evidence

- Command and output: `.codex-tmp/spec182-r11-b9-cross-process-current-20260910-r0/`
- Return code: `rc=1`
- Dynamic dependency capture: `ldd.txt`

## Retry gate

Retry the same unmodified C++ process fixture with the matching NAC-ABE and NDN-SVS
prefixes first in `LD_LIBRARY_PATH`; retain `/usr/local/lib` only after those prefixes
for ndn-cxx/ndnsd dependencies. Count protocol markers only after the Python import and
all native processes start successfully.
