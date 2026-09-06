# T022 — official NAC merge and regression qualification

Status: T022 PASS. Local merge and all required regression gates complete;
no PR/push. The final acceptance section below supersedes intermediate status
paragraphs, which retain the chronological checkpoints and failures.
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

Full NAC CTest passes46/46 cases and4284 assertions in95.24 seconds
(`nac-full-ctest.log`, exit0). All three examples also built. Installed-prefix
focused verification and the clean NDNSF build are ongoing; native/network
authorization acceptance is not yet established for the merged dependency.

Installed-prefix gate passes26/26 cases and1082 assertions, with ldd explicitly
resolving the new installed NAC (`nac-installed.log`). Full merge revision:
`c3aafa6ec5a566879942107c7b20855659c9dfb9`; parents85547eb and58f3948.
Experimental now contains every official master commit (7 ahead/0 behind).

Launcher/guard/evaluator gate: pytest28/28 pass in6.81s (`launcher-pytest.log`).
Two earlier unittest attempts are not acceptance: one has import errors, the
other runs zero cases. The correct runner's count is explicitly checked.

Merged source recovery bundle:
`results/spec179-official-merge-20260905/nac-abe-experimental-c3aafa6.bundle`,
SHA256 `6e844bb7e72c0999660effe869130f6ce43edb5e51a327dd203944aa84634b04`.
Complete history verified; fresh clone has exactc3aafa6 head and tree
`18b082e592379f40367b1f480fb7ef9516ade25e`, clean status and fsck exit0.
The old85547eb bundle and build evidence remain historical and unchanged.

## Clean NDNSF native checkpoint

Build exit0 in28m9.720s at-j2 (`ndnsf-build.log`). `native-closure.log` confirms
all six targets load the new installed NAC without LD_LIBRARY_PATH assistance,
required dynamic symbols exist, and all20 source headers match installation.
The expanded unit gate passes183/183 cases,11998 assertions (`ndnsf-unit.log`).
Integration authorization/stream suites plus the two NdnsfDataV1SvsFlow segment
cases are running. No network acceptance claim yet.

Integration gate passes74/74 cases,1297 assertions (`ndnsf-integration.log`):
the previous72-case authorization/stream gate plus NdnsfDataV1SvsFlow2/2,
16 assertions for independent segments/SVS repair/replay and production
Provider segment context. All native/launcher prerequisites pass. Next is a
fresh18-scenario MiniNDN campaign with clean source and the new exact build dir.

## Final acceptance

NAC `Experimental` is now `c3aafa6ec5a566879942107c7b20855659c9dfb9`, retaining
both the repaired85547eb and official58f3948 parents. The production diff from
85547eb is exactly `src/cache-producer.cpp` and `src/consumer.cpp`; this merge
adds no public header or class-layout change. Earlier Spec179 ABI migration
requirements still apply when upgrading from the unpatched library.

| Gate | Executed result |
|---|---|
| Pre-merge new regressions |12/15 pass,3 failures; ignored CP/KP segment limits and distinct CK-name collision reproduce |
| Merged Producer/Consumer regressions |15/15 pass |
| Full NAC CTest |46/46 cases,4284 assertions,95.24s; three example binaries built |
| Installed NAC prefix |26/26 cases,1082 assertions |
| Native runtime closure |6/6 targets resolve exact new NAC; required symbols and20 installed headers match |
| NDNSF unit |183/183 cases,11998 assertions |
| NDNSF integration |74/74 cases,1297 assertions, including2/2 SVS segment cases |
| Launcher/guard/evaluator |pytest28/28; rejected unittest attempts are not counted |
| Complete MiniNDN campaign |18/18 scenarios,188 scenario assertions, both dedicated User grant gates; driver and all scenario CLIs exit0 |
| Campaign provenance |33 identical artifact hashes match disk; every manifest pins clean `cdd8e55a7d58a6efe16be338b9df137371591285` |

Network evidence is under `results/spec179-official-merge-20260905/campaign/`.
`campaign.exit` contains0. `campaign-verification.log` checks exact scenario
membership, non-vacuous grant controls, per-scenario assertions, clean source,
identical artifact sets and every on-disk hash, including installed NAC f5cb1ec8.

Both Provider grants pass: target17/17 with control32/32, and late target22/22
with control65/65. Both User grants pass: target10/10 with control24/24, and
late target21/21 with control60/60; each fetches exactly one target DKEY and zero
unaffected DKEYs. The remaining14 scenarios cover large-response invalidation,
rotation failure/retry, in-flight and User/Provider identity revocation,
service-scoped revocation, offline rejoin, status retrieval sources, Controller
unavailability/expiry, Targeted refill, stream invalidation, scheduled refresh,
Controller restart, and Selection/Response tampering/replay. Planned Provider
restart and Controller outage yield role exit-2 as expected; other role exits0.

Reproduction commands use the exact paired locations in this report:

```sh
ctest --test-dir /tmp/nac-abe-spec179-official-build/tests -R '^unit-tests$' -V
python3 results/spec179-official-merge-20260905/verify-native.py
build-clang-spec179-official/unit-tests --run_test=ControllerRevocationPolicy,ControllerRevocationState,GenericDynamicApi,RequestScopedConfidentiality,RuntimeStatusStorePersistence --report_level=detailed
build-clang-spec179-official/integration-tests --run_test=Spec175InvocationStream,ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,RequestScopedResponseConfidentiality,NdnsfDataV1SvsFlow --report_level=detailed
python3 -m pytest -q tests/minindn/test_request_scoped_confidentiality.py tests/minindn/test_spec179_provider_grant.py
python3 results/spec179-official-merge-20260905/verify-campaign.py cdd8e55a7d58a6efe16be338b9df137371591285
```

For a new network run, set `NDNSF_BUILD_DIR=build-clang-spec179-official` and
`NDNSF_CAMPAIGN_OUTPUT` to a new, empty result directory before invoking
`bash scripts/spec179_minindn_campaign.sh`; never overwrite the frozen cohort.

This qualifies the measured local pairing and listed fault cases. It does not
prove every timing/deployment or qualify third-party Python/Tiger binaries or
performance. ControllerVersion transitions may cancel in-flight work; Apps own
timeouts/retries. Cached CK packetization changes only on explicit invalidation.
The installed prefix is stable under `.deps/nac-abe-spec179-official`; keep the
old complete pairing for rollback. Next baseline for local work is this merged,
verified pair. T014's upstream publication remains deferred per the no-PR
instruction; importing official changes locally does not publish our fixes.
