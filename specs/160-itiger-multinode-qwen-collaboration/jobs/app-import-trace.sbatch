#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:05:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/app-import-trace-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/app-import-trace-%j.err
set -Eeuo pipefail
SIF=/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif
SRC=${SPEC160_LIVE_SOURCE_ROOT:?}

apptainer exec --nv --cleanenv --containall \
  --bind "$SRC:/source:ro" \
  --env LD_LIBRARY_PATH=/source/native-libs:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/usr/local/lib:/usr/lib:/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu \
  --env PYTHONPATH=/source/vendor-site:/source/pythonWrapper:/source/repoPythonWrapper:/source:/source/llm_pipeline \
  "$SIF" bash -s <<'INNER'
set -x
for mod in \
  ndnsf_distributed_inference.app_sdk.runtime_journal \
  ndnsf_distributed_inference.app_sdk.client \
  ndnsf_distributed_inference.app_sdk.provider \
  ndnsf_distributed_inference.app_sdk.deployment \
  ndnsf_distributed_inference.app_sdk.controller \
  ndnsf_distributed_inference.app_sdk.engine \
  provider \
  user
do
  echo "==== IMPORT $mod"
  timeout 25s python3 -X faulthandler - <<PY
import faulthandler, sys, time
faulthandler.dump_traceback_later(10, repeat=True, file=sys.stderr)
started = time.monotonic()
mod = __import__("$mod", fromlist=["*"])
print("OK", "$mod", getattr(mod, "__file__", None), "elapsed", round(time.monotonic() - started, 3), flush=True)
PY
  rc=$?
  echo "==== IMPORT_RC $mod $rc"
done
INNER
