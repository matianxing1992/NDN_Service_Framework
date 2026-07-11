#!/bin/bash
# Run the deployment lifecycle experiment.
# Requires MiniNDN + coordinator + providers to be set up first
# (e.g., by running NDNSF_DI_NativeTracer_Minindn.py in the background).

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/tmp/ndnsf-lifecycle-experiment}"
REQUESTS="${2:-3}"

export PYTHONPATH="$REPO/NDNSF-DistributedInference:$REPO/pythonWrapper:$REPO/Experiments"

echo "=== Starting MiniNDN harness (background) ==="
sudo -n python3 "$REPO/Experiments/NDNSF_DI_NativeTracer_Minindn.py" \
  --out "$OUT" \
  --runtime-aware-user-planner \
  --requests "$REQUESTS" \
  --concurrency 1 \
  --full-network \
  --tracer-deterministic-runner \
  --enable-native-admission-lease \
  --assignment capacity-pool \
  --provider-check-timeout 60 &

HARNESS_PID=$!
echo "Harness PID: $HARNESS_PID"

# Wait for providers to be ready (check for provision-ready marker)
echo "=== Waiting for providers ==="
for i in $(seq 1 30); do
    if grep -q "PROVISION_READY" "$OUT/logs/"*.log 2>/dev/null; then
        echo "Providers ready after ${i}s"
        break
    fi
    sleep 1
done

# Wait for coordinator
sleep 3

echo "=== Running lifecycle experiment ==="
python3 "$REPO/Experiments/NDNSF_DI_Deployment_Lifecycle_Experiment.py" \
  --out "$OUT" \
  --requests "$REQUESTS" \
  --permission-wait-ms 5000

EXIT_CODE=$?

echo "=== Experiment exit code: $EXIT_CODE ==="

# Cleanup
kill $HARNESS_PID 2>/dev/null || true
wait $HARNESS_PID 2>/dev/null || true

exit $EXIT_CODE
