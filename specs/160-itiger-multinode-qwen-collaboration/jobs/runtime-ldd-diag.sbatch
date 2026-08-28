#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:03:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/runtime-ldd-diag-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/runtime-ldd-diag-%j.err
set -Eeuo pipefail
: "${SPEC160_LIVE_SOURCE_ROOT:?}"
SIF=/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif
apptainer exec --nv --cleanenv --containall \
  --bind "$SPEC160_LIVE_SOURCE_ROOT:/source:ro" \
  --env LD_LIBRARY_PATH=/source/native-libs:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/usr/local/lib:/usr/lib:/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu \
  --env PYTHONPATH=/source/vendor-site:/source/pythonWrapper:/source/repoPythonWrapper:/source:/source/llm_pipeline \
  "$SIF" bash -lc '
    set -x
    python3 --version
    echo LD_LIBRARY_PATH=$LD_LIBRARY_PATH
    ldd /source/pythonWrapper/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
    ldd /source/native-libs/libndn-service-framework.so.0.1.0
    ldd /source/repoPythonWrapper/py_repoclient/_py_repoclient.cpython-310-x86_64-linux-gnu.so
  '
