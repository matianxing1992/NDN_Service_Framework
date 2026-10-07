# Spec193 P03/P04 first runtime failure

Local five-host `p03-p04-diag-001` failed its campaign cache assertion; cleanup PASS. Candidate lock `b3fd3cadfbd3ecf4f30dc9e8dcc54282a7e4ce1c442f88c5a948d940119c639c`, source composition `3b06d1bd9edb1f83d3de69c0bccca3eab20ba1307377350768264e1c62c2410e`, revision `870fff19a6f69971c2e6daa855d3db688433512d`.

Three native ALL publications committed to A/B. The cold unauthorized actual APPProvider invoked the native Repo resolver and received `repo-request-start-failed: Core rejected Provider request`, with no destination. The authorized actual APPProvider fetched model/input/output using `/example/repo/di-authorized-reader`; each native fetch and the APPClient result matched the source size/digest. This is partial runtime evidence, not P03/P04 closure.

The model cache directory is derived from the configured artifact name `/Spec192/DI/model` by the existing APP facade. The campaign incorrectly expected the auxiliary `spec193-model` hint. Correct the test expectation to `Spec192-DI-model`, retain input/output hints, and split cache/inventory assertions. The failed combined assertion short-circuited before final inventory checking. Warm denial and packet qualification were not reached. Product source is unchanged by this repair.

Filesystem exhaustion also interrupted CodeGraph sync and Context Mode sandbox creation. Recover space through regenerable package caches and require successful tool retries before the next candidate. Preserve this failed run and all prior evidence.

Repair verification: the failed run's three actual cache files match the corrected exact paths, sizes and digests. Python compile PASS; CodeGraph retry PASS (five changed files), Context Mode project and strict active health PASS. Approximately 662 MB user-available space remains after cache recovery. These checks do not qualify the unexecuted warm-denial phase.

Next: corrected candidate twice, then remaining A19/A21/range evidence and T007's source reconciliation, affected suites, code-aware audit and full frozen matrix twice. T006/T007 remain OPEN. The overall implementation composition remains uncommitted and cannot yet be reproduced from HEAD alone.
