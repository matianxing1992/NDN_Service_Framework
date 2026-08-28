# Spec 129 Formal Evidence Freeze

**Frozen**: 2026-07-21 CDT  
**Canonical campaign**: `results/spec129-r1-20260721_183058`  
**Status**: PASS, 12/12 cells accepted, each invoked exactly once

Spec 129 and its formal MiniNDN matrix are closed baseline evidence. The
campaign MUST NOT be rerun, selectively supplemented, replaced, tuned, or
reinterpreted through a new output directory. Failed historical repository
guards recorded in `implementation-evidence.md` remain part of the closeout and
must not be edited away to manufacture an all-green historical suite.

Permitted actions:

- read and analyze the canonical artifacts;
- verify artifact hashes without launching a cell;
- use runner `--dry-run` for manifest/schema inspection;
- cite Spec 129 as the baseline for a later independently numbered Spec.

Any new concurrency, contention, partition, crash, timing, or adversarial
experiment belongs to Spec 130 or later and must use a new runner, manifest,
result namespace, and acceptance decision. The Spec 129 runner enforces this
freeze for live execution while this marker exists.

The runner source changed only to install this post-close freeze guard; the
canonical campaign retains its original pre-freeze source and build hashes.
