# Spec184 T007 C++ Process Qualification Refresh

**Date**: 2026-09-11
**Status**: `PARTIAL` / current-candidate process evidence; final qualification remains open
**Build**: `build-spec184-b5-candidate`; target rebuild `r2` completed with `/usr/bin/g++ -B/usr/bin`, `-j4`

本记录把当前候选的 C++ 主导跨进程结果集中绑定到 T007。Python 只负责生成私有
PIB/TPM、启动和回收进程；请求、授权、Provider 执行、ORT 推理、token/数值 oracle
均由 C++ 可执行文件完成。所有 run root 均保留在 `.codex-tmp/` 或 `/tmp/`，没有把
原始秘密或瞬态产物加入 Git。

## Candidate target and runtime closure

The final target invocation was:

```text
WAFLOCK=.lock-spec184-b5 ./waf -o build-spec184-b5-candidate build \
  --targets=unit-tests,integration-tests,DI_NativeRequester,di-native-provider,
  App_ServiceController,DI_NativeArtifactAuthority,spec182-native-grant-requester-process -j4
```

The corrected target command completed successfully; the earlier request for the non-generator
name `DI_NativeOnnxAssemblyWorker` is retained as a Waf task-resolution boundary in the failure
log. The final C++ unit and integration sweeps both exited `0` and ended with
`*** No errors detected`:

| Check | Result | Evidence |
| --- | --- | --- |
| candidate target build | `PASS` | `.codex-tmp/spec184-final-target-build-20260911-r2.log`, SHA-256 `c68038b4fd6973eb55a670907c4bc896bb8f8386cab1064602be714b7cd12fe3` |
| full `unit-tests` | `PASS`, exit `0`, 78.568 s | `.codex-tmp/spec184-final-unit-20260911-r1.log`, SHA-256 `b4691658f3053a51f6b11ca9299e581760ff50b5ede761f5773ad16b5002a6cd` |
| full `integration-tests` | `PASS`, exit `0`, 232.724 s | `.codex-tmp/spec184-final-integration-20260911-r1.log`, SHA-256 `dee9e3b7f5235f5d2c337bf0b98326f9971935116db2616e74c94662d7d6d076` |
| Spec182 C++ harness regression | `PASS`, 71 tests | `.codex-tmp/spec184-python-harness-20260911-r1.log` (`test_spec182_native_closure`, `test_spec182_legacy_exclusion`, `test_spec182_native_bindings`) |

The six business binaries have no `libpython`/`python3` `DT_NEEDED` entry, no matching Python
runtime string, and `ldd -r` reports no unresolved symbol:

| Binary | SHA-256 |
| --- | --- |
| `examples/App_ServiceController` | `50667da49f9f190304fc2de09114b23bc46e0afba4fba143035bd2e5db0940d1` |
| `examples/DI_NativeArtifactAuthority` | `b0840be47108ff78c1241fe480689e167dc175e8bf9ce146b2622be01b3d792e` |
| `examples/DI_NativeRequester` | `7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50` |
| `examples/di-native-provider` | `0a13e7ce70d30be51242b04d252eb0aae11cdbb8abee259bcb2997df71746b18` |
| `DI_NativeOnnxAssemblyWorker` | `e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283` |
| `spec182-native-grant-requester-process` | `02c0e24da191fbb4fa70120862966c895aaa30339bddecd94e752b690b0af853` |

An independent candidate-directory ELF closure check covered the six business binaries above.
`readelf -d` showed no `libpython`/`python3` dependency, `strings` found no forbidden runtime
identity, and `/usr/bin/ldd -r` returned `0` with no unresolved or missing symbol for every binary.
The summary is `.codex-tmp/spec184-no-python-closure-20260911/summary.json`, SHA-256
`36d42fc245f3fd982fad4531cf05eb21e776f6760dd1f46ac59b46ca8f4ed68c`, with overall status `PASS`.

## C++ business process results

Each result below is a fresh run against the current candidate. The process scripts are harnesses;
the named markers are emitted and asserted by the C++ requester, Authority, Provider, or worker.

| Case | Result | Raw C++ evidence |
| --- | --- | --- |
| YOLO protected unary | `PASS`; `NATIVE_NUMERICAL_ORACLE_PASS` values `4,0,12` and `NATIVE_REQUEST_SUCCEEDED` | `.codex-tmp/spec184-process-unary-20260911-r2/requester.log` SHA-256 `bfc3b3276f86522b4be7743932f0aa56b3d70fa2bb1abb19e2e4cbb127665ef2`; Provider log `04ecaa1cdfa3269d2f617b88fde96a428f65b6b706ab20da2fc7c3085390c1a9` |
| Qwen tiny stream | `PASS`; 8 token events, terminal oracle and request success | `.codex-tmp/spec184-process-stream-20260911-r1/requester.log` SHA-256 `b4234811675655641bab0923f26d19795b340d954a1285a0760f50106510c4ba`; Provider log `e478a8c21c8573d844dce775f4612515aa3677fa0f5e7e9da687b5ab530686cb` |
| two-turn continuation | `PASS`; checkpoint written, second turn succeeded, wrong parent rejected | requester logs `3357d1303855a3e486dbb2712c4e17b7d249561d128f51a865d040525199994d`, `779fda2109f2dd2a2d78c6da85911a2b747aef44af1a3f0998e0f7ba8ecb2b00`, `cafe6b9e31a2d4676d9ab384514cebbc2d6e5818439eaba98c49833b2bf1871c` |
| Provider recovery | `PASS_FOR_ROW`; first turn succeeded, Provider restart caused `PROVIDER_CONVERSATION_STATE_MISSING`, wrong parent rejected | `.codex-tmp/spec184-process-recovery-20260911-r1/`; requester hashes `c183a8d1d8ce000ad20509fbcbf70a39017810392f59714d8c2af6c3e245b52c`, `3a47df8022cfeba178820cc1f9abe1508d88ee5d68609f417f448b2c93c55c47`, `f1e8024d92c67a8f96727346132cfdf6d17fefa8fa8135c09aad213961950be1`, restart hash `e1ed7d48732d1ed22e0498e7bb99f20dc990519fa2e5c3d43585958d5311a9d8` |
| Provider replacement | `PASS`; first Provider stopped before execution and Provider-B completed attempt 2 | requester `fc3c904684b811775d04f5073c9e730b4517362f8cc346023fe69b1b3f1501ed`; Provider-B `2e177dd0ef2ef61b41b3e8767863afae394dc644759558b023c62b41916f5aa4` |
| replacement without backup | `PASS_FOR_ROW`; terminal `DI_NATIVE_NO_ADMITTED_PROVIDER` | `.codex-tmp/spec184-process-replacement-nobackup-20260911-r1/requester.log`, SHA-256 `461893379d9d696f308b81c9e81f50a3be05f67fe22f2e06493e9519a84108f6` |

The replacement run's first Provider log is `d8cea73452cdca5ce860e424881445d62eda39c14e2123738a0f360d9ccc9a98`;
it contains no execution evidence, while Provider-B contains the authenticated attempt-2 evidence.

## Candidate-bound C++ dynamic behavior samples for I02-I08

The following owner runs use the same `build-spec184-b5-candidate` integration binary and
candidate-staged native fixtures. They are dynamic behavior samples for the C++ selectors; they
are not substitutes for the inherited isolation counterexamples or collector-completeness cases
defined by `native-isolation-design.md`.

| Case | C++ business oracle | Owner result SHA-256 | Runner result SHA-256 | Node context SHA-256 | Outcome |
| --- | --- | --- | --- | --- | --- |
| I02 | `ackCandidates=2 ackClosed=1 planCommitted=1 streamedContext=1 completed=1 events=4` | `452f74eb6919b97db24f5db321f4f5681869625753f19364989f88ba48eec524` | `8939d535f32950b75de1bda6973e3fc6948cc143b52f37fc9fbb92a1990ab934` | `feec5f95b63576f7dee12ac3e20218bdd96f5acb7452342bd1511900afd35f16` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I03 | `ackCandidates=4 ackClosed=1 planCommitted=1 streamedContext=1 completed=1 providerCompletions=4 events=8` | `6a824cee3f4a4946a14c9b46bc62f7959e9a8d9c091f2e93839b0c19612c1f07` | `77c03920f469a145f29a8a9bca57d25df6d2fc3484fa556d54bcce4b1e31f33c` | `0626a7fb8c45ae2621a2ea3f1eec6dac512055f9528da6fd9dbd806fa6bcd553` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I04 | `completed=1 publicationReorders=1 events=8` | `66fb4f924ac3debe6c7487c1dc033fdf3a31157c62563cc7d1c73d8ec6f0adf7` | `008624d79bec2e41e45adca54ec05c699e91993b8309df302773c86b590004ca` | `485a91a87f1de5ba6d26a26451e99fe8e0a158258748a8451084a295e8a25029` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I05 | `completed=1 publicationDuplicates=1 events=8` | `aa4ddd3f97f4d83e1b6ce54e474f5b056baee239ac0ae069e933782604a5f5fb` | `66304a51876f7e8fbb27956bb52fa855b503acb32faf58867b0eee5cec144025` | `5dced25a360ffdd61a891eda4201837a2eb41d9312e37cafc61071d56536ed6c` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I06 | `completed=1 droppedData=1 events=8` | `a02df6e9e3cf7ec4fc262517843126d2b2714b04d45c582e11f0f0d8c31a3039` | `c6d242ee4bafb5531259daa0273f7029efa7d96d31a4d0210ba5ebcc74885fa0` | `c9c6b4f84bd3e397f38b038e1ebfe70637c425515be4d6b07937c06bf30e7da4` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I07 | three C++ negative subcases: never-retained, retention-expired, end-before-gap | `bfa6621e9504aef2b4f6a78e1d1bfd46169854f6b00d46e9b6483b73f58cb693` | `aff02336cf661f6a941f8d300d9f76f2951791e7d15e85b89d7b81d4d05a985b` | `f91bf95c87961415bcb6400cf3416d40b59b00830d387e42aab5e8bae76a10a9` | `PASS_FOR_DYNAMIC_SAMPLE` |
| I08 | `cancelled=1 completed=0 failed=0 events=3` | `fe5c0817113729597ce59bca71ba3fd5436546b389a66ade8a6ef32068321cb9` | `9b0462c49e78fcca3af3e7cbdaf6c512d304577fc26a5928b48fe960dee24bc7` | `ccf0c6b4014f4f037a5c9b076a23a19429ebb0356d6a76d7b20f87581b709007` | `PASS_FOR_DYNAMIC_SAMPLE` |

The complete raw run roots are `.codex-tmp/spec184-owner-i02-20260911-r4/` through
`.codex-tmp/spec184-owner-i08-20260911-r1/`; each valid result has `evaluation.status=PASS`,
`observation.complete=true`, all seven required evidence categories, and no policy or integrity
violations. The I02 retry history remains durable: `r1` stopped at the missing `ifconfig` runtime
path, `r2` at missing staged fixtures, and `r3` at an invalid manifest artifact kind. Those are
harness boundaries, not protocol results.

## Candidate-bound C++ isolation counterexamples

After strengthening the canonical collector, a dedicated C++ fixture exercised the inherited
I02-I08 counterexamples under the real MiniNDN owner. The fixture source is
`tests/standalone/spec182-native-counterexample.cpp` (SHA-256
`d8467fb94ec08a1a51f27d595b2acdb74f1dbe0d1f73154a8b733f1ae6345600`); its candidate executable
has SHA-256 `ff37711aa90e3f1e5916881e03ea405b1252c0ab1c1acf79d6779e29af3540c2` and contains no
`libpython`/`python3` identity. The collector source hash is
`3022d14fa413ed128203b46ad592cf5cc0a18903eae5c34afffd052990142658`.

| Case | Expected registration status | Observed status / boundary | Owner result / runner / trace SHA-256 | Raw run root |
| --- | --- | --- | --- | --- |
| I02 | `FAIL` | `FAIL`; renamed in-root helper exec → `UNDECLARED_EXEC` | `3eaf1e205c3270fc0fde5208830eea9d595b806271813edfaa7dc6b03d960c1e` / `d2d455bec466f904484feb35fce221c158a254bd8a1c075e86bc8ff67e4ba9c2` / `da28711541f6004e88e860a29fb37f0a280d3aa027d6228904afddc86dfa3a21` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i02-r3/` |
| I03 | `FAIL` | `FAIL`; C++ `dlopen` opened staged `libpython3.8.so.1.0` and detector reported `PYTHON_MAPPING` | `227cd85b6b7facd56e64cd4603c7f4a70c7aebcefe7849ba9060f05a56233e47` / `f5ff86ca4cf1c405308f940f87782f6c8eff4f01d61d32a064fc20799493ccd5` / `a6447f00ab6f82d03b664361dcb5e7e7497ed3ddbcd1aa27c0190e94390b2a0a` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i03-r4/` |
| I04 | `FAIL` | `FAIL`; failed loopback connect attempt → `UNDECLARED_ENDPOINT` | `2aeb6cbe25bb3315ff0838a944d25e0f8d8a652811659885dd90d3d5a6660297` / `5b093d10f45126144c2deba535733d7737ccc2a4bdbc7c7fff61a6b60064706e` / `0a2c9b799b84dc5fa99e181a55f95580d72c6856fd8da317514e9115e030b72b` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i04-r3/` |
| I05 | `UNQUALIFIED` | `UNQUALIFIED`; trace exceeded the declared 4096-byte observation budget → `TRACE_BUDGET_EXCEEDED` | `6d401fe8c3a786e9ef68fa0037a72225b381ec77b041a7cbe3147887e0397de2` / `d770310117daa39b3cad73f111d5495630e61fb59c98e5e2f1dcbb379b00e086` / `c37c029024c732b73bcd71e87de9d9a942186bfe14127aa841a8bb42307ae1bb` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i05-r3/` |
| I06 | `FAIL` | `FAIL`; C++ cold case omitted the required Provider role → `ROLE_COVERAGE_MISMATCH` | `7ccbf90b6e3b2037a8c88cbb9315c3899314fcbeda52c8937220d2354383b283` / `c12244097810a8e46afed8d56721a1870d08ed0d23a2c65a246e86f189951fd9` / `a19bf6e273ddc7d98ce981df51608c4f20ebe0fe4bc95e7199d9f9235c9d00f3` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i06-r3/` |
| I07 | `PASS` | `PASS`; Python owner remained outside the closure and native C++ business marker passed | `1048eafb7921ea25b87bca15f44a7c174c012aa60b7df6d801a5be99df00661c` / `122fd44f291b6f4f8c74d7475c5bfb3a6d1f06ca04b7869ac5b1e55901557d8f` / `5396ea92037f213bbfc3f1b3fa00d9a64d22c1c1187eeaa45191f7eccb257b5a` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i07-r3/` |
| I08 | `FAIL` | `FAIL`; detached descendant was killed during cleanup and remained an owned descendant observation → `OWNED_PROCESS_ALIVE` | `ac870f8b25d349726c6d4eb599344e78e3ffcab0743126497f1899478efad9ed` / `e14eebb6537fc5083cee87938fc22c541557b1b5943fdf84d5e252484279492f` / `81ef6eab24a2b7b0eb9d71779517e2d59a9e07d57623036c06d1543dd10b839e` | `.codex-tmp/spec184-counterexamples-live-20260911/owner-i08-r4/` |

All seven owner results used the canonical topology, explicit `/usr/bin/bwrap`, `/usr/bin/strace`
and `/usr/bin/nsenter`, and fresh output directories. The C++ fixture emitted the business marker
before each observed policy boundary; the negative status comes from the collector/evaluator and
not from a Python oracle. I05 intentionally remains `UNQUALIFIED`, because an observation failure
cannot be promoted to a protocol `FAIL`.

## Static review and detector batch record

The bounded detector batch was reviewed with the official `review-agent` profile at
`/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) and the project
pre-test gate. The review covered five lanes: the MiniNDN entry/runner call path, collector and
evaluator implementation, the C++ counterexample fixture and Python regression selectors, process
registration/build closure, and candidate evidence/matrix/task wiring. It found and checked the
following boundaries before the owner reruns: unfinished/resumed `execve` path pairing, failed
undeclared `connect` attempts, trace-budget overflow, terminal descendant observation, and the
distinction between an observed policy `FAIL` and an incomplete-observation `UNQUALIFIED` result.
No unresolved static finding remained after the review. The review did not run tests; the focused
Python and C++ checks below are the separate post-review validation gate.

| Reviewed artifact | Bound source/selector | Result |
| --- | --- | --- |
| canonical collector/evaluator | `tests/standalone/run-spec182-native-closure.py` SHA-256 `3022d14fa413ed128203b46ad592cf5cc0a18903eae5c34afffd052990142658` | `STATIC_PASS` |
| C++ counterexample fixture | `tests/standalone/spec182-native-counterexample.cpp` SHA-256 `d8467fb94ec08a1a51f27d595b2acdb74f1dbe0d1f73154a8b733f1ae6345600` | `STATIC_PASS` |
| regression selectors | `tests/python/test_spec182_native_closure.py` SHA-256 `0604f301277097af1bf777f858316d016e85f8f9ffcb59dd2700ec9f9f92232c` | `STATIC_PASS` |
| task/matrix/evidence wiring | current Spec184 `tasks.md`, `contracts/qualification-matrix.md`, and this record | `STATIC_PASS` |

The owner runs in the preceding table are the runtime gate for these findings. The fixture is a
test-only C++ executable and is not a production binary; its source and binary identities are
bound in the candidate record. Python remains only the staging/collector harness and regression
test host.

## Current-candidate Spec175 C++ integration gate

As a second C++ process-level sample, the current candidate `integration-tests` ran the registered
Spec175 I01–I15 selectors once each with seed `1840012`. The gate reported `PASS`, all 15 results
had exit `0`, and no case was missing. The executable digest is
`sha256:e1947cfb5d75524edc346da5b9c32c984c2daf8e3504e095653e13201486a385`; the gate manifest is
`.codex-tmp/spec184-spec175-g2-20260911/qualification-manifest-current.json` with SHA-256
`635c2c3db30ee44f08be28b6d4c62ee4272a7e012514124bcd1ab21158cde910` and the current source-seal
SHA-256 `sha256:2f4b9680180e999988ad0f4d149e8d39a1905585a771a50f096a75837aa977d9`.

This gate supplements the owner runs with I09–I15 C++ behavior classes. Its source seal records the
pre-existing integration test marker and an unrelated untracked extension-build log; those files
remain outside the promotion candidate's production source identity. The gate therefore provides
candidate executable and test evidence but does not erase the inherited I02–I08 counterexample or
collector-completeness boundaries.

## Authority grant process matrix

The same candidate's independent C++ Authority/requester process launcher completed one positive,
five Authority-handler rejection cases, and one transport-unreachable case. Every launcher reported
`SPEC182_NATIVE_GRANT_PROCESS_ISOLATION=PASS`; the expected negative cases returned their declared
native rejection marker rather than a Python oracle result.

| Case | Native boundary | Probe log SHA-256 |
| --- | --- | --- |
| `positive` | signed grant accepted | `6d8e95dd795b7dc6f33ddc74c6418dc1228f4c7a2dd9dc04be1c4c46b9084962` |
| `bad-signature` | Authority rejects signature/policy | `531e7065f082f437deec6b25b502b079a14696af9435d58677f1a053be728920` |
| `wrong-epoch` | Authority rejects epoch/policy | `b053b7166bb4991cf06793f02ead7baccdf770f0509760286499c96892e9064d` |
| `unknown-recipient` | Authority rejects unconfigured recipient | `cef7bedd36a95415647af2a729ce9d648e75f554a0cd35a928b3f97f303845b7` |
| `malformed` | Authority rejects non-canonical request | `1637ba022c60598b6cf6fdb87321bb1f0acc79dafe0d1085c85f32413e8a964c8` |
| `expired` | Authority rejects expiry beyond policy | `3fe8a378ae09a3029fc8b093691869b65a53d45d71a91763329dbf788b48b3c3` |
| `unreachable` | requester reports transport timeout | `f5123c684d83dce257096e807712ea773ac8e52776a909222a9bc5d6676a9a8f` |

The retained grant run roots are `/tmp/spec182-r11-b1-process-8d0naebx`, `e8sipn0i`, `tq52nfgq`,
`24l14sex`, `do_j5695`, `9g6mi4ji`, and `mn4xqjn5`, in the table order above.

## MiniNDN owner and isolation evidence

The canonical two-node MiniNDN owner was rerun with the refreshed runner manifest
`.codex-tmp/spec184-b5-current-runner-manifest-r3-20260911.json` (SHA-256
`a1e7df48caf5837d690ad1847e3490704c309af739e0df318ef3062ac8fdfd13`). The current root run
`.codex-tmp/spec184-owner-po001-20260911-r9/` returned `PASS`, exit `0`; its result digest is
`e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd`, runner-result digest is
`d3f757c37e196de6e616554e65eaea324b3e8706efe142c2d9cd8f673ccedcd0`, and node-context digest is
`97d548d3a12e91718400faf604629f11fb2bbf44e0d1338d73ddc1d1be4e625f`.

The runner observation was complete with no policy or integrity violations and all required
evidence categories (`identity`, `process-tree`, `namespace`, `exec-map`, `endpoints`,
`business-oracle`, `cleanup`). No `PYTHON_EXEC`, `PYTHON_MAPPING` or undeclared endpoint was
observed. The non-root owner boundary and the earlier stale-manifest/digest attempts remain
historical `UNQUALIFIED` records; they are not reclassified as protocol failures.

## Qualification disposition

These results close the current candidate's bounded C++ process classes for unary, stream,
continuation, recovery, replacement, grant authorization, the candidate-bound I02-I08
counterexample statuses and the `PO-001-stream` owner row. They do not make I05 an accepted
protocol result, nor close broader real-model, Python-retirement or external SIF/Tiger ownership;
the matrix keeps those rows `PARTIAL`/`OPEN`. T007 therefore remains `PARTIAL`; T008 remains
blocked by the qualification dependency, not by the process runs recorded here.

## Targeted C++ suite boundary

The first combined current-candidate selector for the native planning, preparation,
offer, provider, tokenizer, conversation, placement and grant suites ran 151 cases
but stopped at `Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint`
after `DI_NATIVE_OFFER_REJECTED`, followed by a memory-access violation. The raw output
is `.codex-tmp/spec184-b5-targeted-unit-20260911/unit.log` (exit `134`). This is retained
as a failure boundary, not a qualification result.

The named placement case passed in three isolated fresh runs, and the same 151-case
selector passed in three fresh reruns. The order-sensitive boundary remains subject
to sanitizer or repeated stress validation before promotion; T007 stays `PARTIAL`.

The full current-candidate unit sweep was then rerun with the required
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` environment. It ran to completion with
exit `0` (`.codex-tmp/spec184-b5-full-unit-rerun-env-20260911/unit.log`, SHA-256
`b3e2f457d26fe019760f124cb225a65414cca2bc568c0603c4fc246fbd5686cd`, elapsed `1:49.15`,
maximum RSS `8396500 KB`). The earlier exit `201` full sweep without this variable is retained
as a setup/configuration boundary and is not combined with the passing result.

The same environment was used for three repeated 151-case native planning,
preparation, offer, provider, tokenizer, conversation, placement and grant selectors;
all three exited `0` and reported no errors. Their logs are
`.codex-tmp/spec184-b5-targeted-unit-env-20260911/unit-1.log` through `unit-3.log`,
each SHA-256 `66efc8f7b0015b31c337a11daabe3a34a3f4f1dcfba497f2c822949ab0586847`.
This repetition did not reproduce the earlier order-sensitive failure, which remains
recorded as a separate boundary.

The full current-candidate C++ integration sweep was also rerun with
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`. It completed with exit `0` after
`4:09.61` (maximum RSS `114048 KB`); the raw log is
`.codex-tmp/spec184-b5-full-integration-rerun-env-20260911/integration.log`, SHA-256
`e679f2d0ace15827dfd7a5a423660965b0e583df65ba31d8b72370c65b846966`. The earlier
environment-missing run remains a setup boundary; this result is the candidate-bound
integration PASS and still does not close external/model/retirement rows.
