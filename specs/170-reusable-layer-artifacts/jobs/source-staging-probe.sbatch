#!/usr/bin/env bash
# Non-gate source staging probe. It verifies the exact archived Experimental
# candidate on a compute node; it does not build a container or run inference.
#SBATCH --job-name=spec170-source-probe
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=2G
#SBATCH --time=00:05:00

set -euo pipefail
: "${SPEC170_SOURCE_ARCHIVE:?}"
: "${SPEC170_EXPECTED_COMMIT:?}"

scratch=$(mktemp -d /tmp/spec170-source-probe.XXXXXX)
tar -tzf "$SPEC170_SOURCE_ARCHIVE" >/dev/null
tar -xzf "$SPEC170_SOURCE_ARCHIVE" -C "$scratch"
test -f "$scratch/ndn-service-framework/ServiceProvider.cpp"
grep -Fq 'hybridCompactKeyId' "$scratch/ndn-service-framework/ServiceProvider.cpp"
test -f "$scratch/specs/170-reusable-layer-artifacts/spec.md"
printf 'SPEC170_SOURCE_PROBE_PASS|commit=%s|archive=%s|sha256=' \
  "$SPEC170_EXPECTED_COMMIT" "$SPEC170_SOURCE_ARCHIVE"
sha256sum "$SPEC170_SOURCE_ARCHIVE" | awk '{print $1}'
