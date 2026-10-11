# B6 Source, Binding and Runtime Closure

**Members**: T007/T008 surface checks feed T009; one master, read-only fixed-snapshot review. No current network qualification.

## Design binding

`tests/minindn/spec193_preflight.py` retains its shared filename and `check(...)` signature. Active authority becomes Spec202; design inputs reference its accepted contracts, validation matrix and current inventory. `source_candidate(root)` owns only a deterministic file inventory/hash, not Repo business state. The runner's existing `source_candidate()` delegates to it. Include clean and modified participating framework/Repo/DI/binding source, selected native fixtures, imported MiniNDN peers, schemas, policy, Waf/build entrypoints. Missing required inputs and tracked deletions fail before output creation. Ignore generated binaries, caches and evidence; unrelated Tiger/UAV implementation is not silently absorbed.

Source hashing alone is not source/artifact closure. A build receipt must bind the before/after-build source hash, effective build configuration, native artifacts, both Python extensions and loaded dependencies to actual successful rebuild commands. Preflight rejects an absent/stale receipt or mismatching artifacts. Fixed common source and explicitly declared 5/6/7-host/object/import profile locks remain separate; changing a profile cannot reuse another profile lock.

## Closing gates

Read-only five-lane review before dependent execution; source inclusion/deletion/mutation and build-receipt negative probes; current public native suites and Python conformance; actual production CLI import/reopen/parity; unsuppressed ASAN/UBSAN. No Python fixture models C++ behavior. Native codec behavior changes require bounded C++ parser fuzz; inventory-only verification does not invent that requirement. T010 waits for fresh convergence with all controlling source/runtime gaps closed.

## Initial findings

The inherited preflight hardcodes active Spec193 and only hashes dirty/untracked source. Committing a participating source file therefore removes it from that candidate inventory. The inherited source-files loop silently skips missing paths. These are T009 admission defects, independently of the already-green focused native repairs. Existing runtime artifacts and old locks do not prove current Python/native source closure.

**State**: DESIGN_READY for the bounded preflight repair; whole T009 remains NOT_VERIFIED until build receipt, affected suites and final audit close.

## Bounded preflight closing checkpoint

v1 `preflight-v1.zip` SHA a6f0d2b82c7bde93c373e3d0b182ed558f60db0004543ec4fc7e886e45cee562 was BLOCK: selected fixture deletion and staged deletion could shrink the inventory. v2 SHA a72fa59c48479f9888eb688ee1a1e37f880939957c4b15df2947b76345848b31 uses the common participation predicate plus HEAD/index/current inputs; all missing participating inputs fail closed. Current source hash/list are recomputed in `check`, and source rejection reaches structured preflight output. Actual generic integration fixture cpp/hpp are mandatory. Cross-run durable roots now belong to Spec202.

Read-only five-lane v2 review PASS. `mutation-tests.log`: 12 real-environment tests, exit0, 77.330s; clean-driver byte mutation, absent source, omitted/stale hash/list, selected/staged deletion, binary/design lock mismatch, roles/ACL/policy/modules and invalid production mode are checked before output creation. This closes the bounded source-admission slice, not source/artifact freshness. Build receipt and final qualification remain pending.

## Build receipt binding

`tests/minindn/spec202_build_candidate.py` will be the bounded build orchestrator, not a second installer or Repo implementation. It requires exclusive source during the build, a fresh ignored build directory and fresh evidence output. It snapshots participating source and declared external SDK inputs before/after, configures the actual Waf targets against the pinned system-Boost1.71 SVS SDK, builds unprivileged at most j2, uses the existing selected-target Waf install only for Core/DI and public headers, then rebuilds both wrappers using their existing setup.py and required installed-library digests. No dependency source rebuild, new Boost prefix or Python business state is introduced.

Native runtime targets are copied from that fresh output into the harness's existing `build/` paths with equal-byte hashes; original outputs are recorded, not claimed newly compiled. The receipt binds source before/after equality, actual configure/build/install/binding/import commands and exit0 logs, fresh Waf cache/config, native outputs, installed Core/DI, extension binaries, external SDK headers/config/toolchain and resolved runtime libraries. Actual module imports must load the declared Core/DI/SVS bytes. Ordinary tests use the freshly built outputs. The preflight consumes `build/spec202-source-build-receipt.json`, rejects missing/stale/incomplete inputs or artifact/config/dependency mismatch, then creates its profile lock; it never manufactures a receipt.

Git revision remains provenance metadata. Candidate equality is controlled by source/config/design/artifact/dependency bytes and declared profile, so a progress-only checkpoint outside those inputs does not invalidate runtime identity. Acceptance-bearing Spec/plan/tasks changes remain explicit inputs and require their scoped gate. Whole native/public/binding/CLI and unsuppressed dynamic gates still precede T010.
