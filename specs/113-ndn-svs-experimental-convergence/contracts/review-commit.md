# Contract: Reviewable Commit

Each product commit must provide:

- one concern: mapping, recovery, publication, or build baseline;
- the original source commit/diff lineage;
- requirements and failure mode addressed;
- production code and its directly relevant tests together;
- a clean `git diff --check` result;
- a concise subject that describes stable behavior, not "experiment" or WIP;
- an independently describable revert effect.

Prohibited content:

- `.gitignore` rules for agents, local specs, results, or ignored docs;
- Spec Kit/GSD state from the NDNSF control repository;
- unrelated NDNSF-DI/UAV/Repo/container work;
- superseded reviewed implementations from old local history;
- mixed Spec 110 recovery and Spec 112 segmentation changes in one commit.

The final sequence is validated both commit-by-commit and as one integrated
tree. A commit that passes alone but breaks a later required concern is not
accepted until the full sequence passes.
