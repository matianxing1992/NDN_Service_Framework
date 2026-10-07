# Spec193 Python, DI and legacy local closure

T006 is DONE_LOCAL: P01–P06 have completed their local five-host batches twice. Spec193 overall remains OPEN. The full current-source audit, affected suites, source reconciliation and complete frozen matrix twice are separate T007 requirements.

| Cases | Local evidence | Result |
| --- | --- | --- |
| P01/P02 | `p01-p02-diag-001/002`; [cross-language closure](spec193-cross-language-local-closure.md) | Native references/receipts reused across actual C++ and Python network clients |
| P03/P04 | `p03-p04-diag-002/003`; details below | Actual new DI Providers, authorized reads/execution, cold/warm native authorization rejection |
| P05 | `p05-diag-002/003`; [legacy import closure](spec193-legacy-import-local-closure.md) | Production native importer, independent consumer reads, source DB/WAL/SHM/CAS unchanged |
| P06 | `p06-diag-001/002`; [retired Python closure](spec193-retired-python-local-closure.md) | 141 retired entry points reject with exact versioned contract and zero Repo side effects |

P03/P04 uses the Persistent profile without the importer or object-protocol fixture server. Both runs have identical candidate lock `10ada7658768536554de83c5f2b103958b2e54070359bea09c94b06757d7e067`, source composition `9916b04996ab94ce0dfe7cb87570b7d52a6fcf47a92242842a559ff6c078d71c`, revision `0fb0ba3ec845c126ed07c866c460ae23f8da0cfd`. The replay supplied diag002's lock. All 173 locked files were physically rehashed after both runs, with no changes.

Three C++ ALL publications commit model/input/output to both Repo A and B. Only native handle projections are passed to the DI peers. The real APPProvider and APPClient execute role Inspect using `DIRepoArtifactResolver` and the real native Repo client; the handler inspects materialized bytes. This is functional DI reuse evidence, not an ONNX inference benchmark.

| Slot | Bytes | SHA-256 |
| --- | --- | --- |
| model | 37 | `0faf4dac2e81991fe6f6943b9fff7cbc4571b2d5f9a9c74ca67aaba22aaf5a7b` |
| input | 37 | `b065546a6bbd6ca9c3ba8bb1130cf8d4e0b9c9fb03d83abea62b9083d44a70b2` |
| output | 38 | `4bc81126c1b7fd205c70d38852540eab111ad0b0a577c8b7f423c27c56193268` |

Each run tests cold unauthorized Provider, authorized Provider, then another fresh unauthorized Provider with the authorized cache still present. The DI service grants both Provider identities, but the separate unauthorized Repo reader has no Repo grant. Both denied phases actually enter the native resolver and report `repo-request-start-failed: Core rejected Provider request`; destinations are absent, the DI handler never completes, and denied workspaces contain no files. Cold and warm denied epochs differ. The client returns the expected failed inference result and exits0.

Authorized native fetches and the actual DI result match all three digests/sizes. Model cache naming follows the existing facade's `/Spec192/DI/model` artifact name; input/output use their auxiliary hints. Three exact cache files survive warm denial unchanged. Both Repo artifact inventories stay equal to their post-publication baseline. The two runs' cache inventory digest is `6836533b4e4d08b8405c1b550845cd02f1537b1500108462a3f3b7cdf8cda0ef`.

Each run has five nonempty captures and 18 independently verified Repo Request/ACK/Selection/Response chains. The oracle verifies the extra authorized reader certificate against that run's root, inner/outer signatures, namespace signer and identical wire at caller and selected Repo. Required operations are USER STORE/STATUS and authorized-reader STATUS/FETCH_RANGE. The unauthorized reader's Request namespace has zero matching packets across all five captures. DI execution/epochs are actual application evidence; these 18 chains refer specifically to Repo traffic.

All seven finite peers exit0. Three C++ publishers hide Python in PATH and have actual mapping observations showing no Python. Python DI/binding peers are separate. All Provider stops are bounded and both final MiniNDN cleanups PASS. NDN trace logging is disabled by the launch and child environment.

Diag001 is retained as FAIL/cleanup PASS: the test incorrectly expected an auxiliary hint to name the model cache. Correcting the campaign expectation and splitting cache/inventory assertions fixed it; product facade behavior stayed unchanged. See [first failure checkpoint](spec193-di-reuse-first-failure-checkpoint.md), `docs/failure-log.md`, and the private Spec193 `contracts/di-reuse-campaign.md` / `evidence/p03-p04-review-20261007/review.md`.

Context Mode project/strict-active health and CodeGraph sync recovered after disk exhaustion; package-cache cleanup retained models and run evidence. Spec Kit owns the contract, review and task/coverage updates. Resumable state continues in the Spec registry. ARS research/statistical workflows are not applicable to this functional batch.

Next: close larger Persistent range transport, A19/A21 evidence and O17's unified candidate coverage, then complete T007. Existing P batches use distinct frozen candidates/profiles; they do not form the final matrix qualification. Implementation source is still uncommitted and HEAD alone cannot reproduce the complete source composition. No TigerCluster or performance qualification is claimed.
