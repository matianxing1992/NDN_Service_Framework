# T022 — official NAC merge and regression qualification

Status: in progress. User authorizes local merge, explicitly no PR/push.
Starting pair: NAC85547eb, NDNSF5dfc1795. Official target58f3948.
Prior installed prefix `/tmp/nac-abe-spec179-exact-prefix` and campaign evidence
remain frozen. Results root: `results/spec179-official-merge-20260905/`.

Pre-implementation audit passes: two-file official merge, retained NAC
security/lifecycle changes, regression-first non-default segment limit,
real segmented retrieval and both-role Controller authorization qualification.
Context Mode project/active health, CodeGraph caller lookup, Spec Kit and GSD
are used. NAC has no CodeGraph index. ARS not applicable to correctness tests.

## Pre-merge red

Fresh Clang10-j2 build on85547eb plus four new cases. The first build exposed
test-only protected factory access; explicit Data templates fix compilation.
`nac-segmentation-red.log`:15 cases,12 pass/3 fail,5 failed assertions, exit201.
Both CP/KP CacheProducer tests observe1500-byte content despite requested128,
and CK packets remain unsegmented. Distinct CK object names ending in typed
segment components collapse through normalizeCkKey: the second consume returns
incorrect plaintext after a cached first success. The segmented object/exact
segment retrieval compatibility test already passes. Existing invalidation and
reentrant callback tests pass. Official merge is started with no conflicts;
green qualification pending.

## Merged dependency checkpoint

NAC Experimental merge commit `c3aafa6` has both85547eb and official58f3948 as
ancestors. Only the official two runtime files changed; all prior security
repairs remain. The four new regression cases plus existing Producer/Consumer
cases pass15/15 (`nac-segmentation-green.log`), including correct distinct-CK
plaintext and CP/KP cold/warm/clear packetization. Full NAC CTest is running.

Fresh install: `.deps/nac-abe-spec179-official`, ignored and isolated from the
previous prefix. Installed lib SHA256
`f5cb1ec8da43b57b165837287f57291887fb521a9d72fe4d729184fdff116d8a`;
its ldd closure has no missing dependency. Clean NDNSF configuration succeeds
in `build-clang-spec179-official` using Clang10/system binutils, debug-Werror,
shared library/tests and the exact new prefix. Native build starts at-j2.
