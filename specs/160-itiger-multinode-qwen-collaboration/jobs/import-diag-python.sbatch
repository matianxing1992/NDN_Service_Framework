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
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/import-diag-python-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/import-diag-python-%j.err

set -Eeuo pipefail

SIF=/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif
SRC=${SPEC160_LIVE_SOURCE_ROOT:-/project/tma1/ndnsf-di/src/spec160/qwen-live-v3}

apptainer exec --nv --cleanenv --containall \
  --bind "$SRC:/source:ro" \
  --env LD_LIBRARY_PATH=/source/native-libs:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/usr/local/lib:/usr/lib:/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu \
  --env PYTHONPATH=/source/pythonWrapper:/source:/source/llm_pipeline \
  "$SIF" bash -s <<'INNER'
set -x
python3 -V
/usr/bin/python3.8 -V
/opt/venv/bin/python3 - <<'PY'
import sys
print("py310", sys.version)
for name in ("torch", "transformers"):
    try:
        mod = __import__(name)
        print(name, "ok", getattr(mod, "__version__", None), getattr(mod, "__file__", None))
    except Exception as exc:
        print(name, "failed", type(exc).__name__, repr(exc))
PY
/usr/bin/python3.8 - <<'PY'
import sys, sysconfig
print("py38", sys.version)
print("ext", sysconfig.get_config_var("EXT_SUFFIX"))
for name in ("torch", "transformers", "ndnsf._ndnsf", "ndnsf"):
    try:
        mod = __import__(name, fromlist=["*"])
        print(name, "ok", getattr(mod, "__version__", None), getattr(mod, "__file__", None))
    except Exception as exc:
        print(name, "failed", type(exc).__name__, repr(exc))
PY
ldd /source/pythonWrapper/ndnsf/_ndnsf*.so || true
INNER
