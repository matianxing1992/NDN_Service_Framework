#!/usr/bin/env bash
# Runtime-only helper for install_ndnsf_stack.sh; no native build or network protocol.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime="${1:?runtime destination required}"
base_python="${NDNSF_PERSON_PYTHON:-python3.10}"
wheels="${NDNSF_PERSON_WHEELS:-}"
lock="$root/NDNSF-UAV-APP/tracking/requirements-person.lock"
"$base_python" - <<'PY'
import sys
if sys.version_info[:2] != (3, 10):
    raise SystemExit('CPython 3.10 required')
if sys.prefix != sys.base_prefix:
    raise SystemExit('Select a stable base interpreter, not another virtual environment')
PY
if [[ -e "$runtime" ]]; then
  # Reuse only an exact locked environment; never overwrite another environment.
  "$runtime/bin/python" "$root/NDNSF-UAV-APP/tracking/person_runtime.py" --lock "$lock"
else
  [[ -d "$wheels" ]] || {
    echo 'Set NDNSF_PERSON_WHEELS to the verified wheel directory' >&2
    exit 2
  }
  "$base_python" - "$wheels" "$lock" <<'PY'
import hashlib
from pathlib import Path
import re
import sys
text = Path(sys.argv[2]).read_text()
entry = re.search(r'^torchreid==1\.4\.0 (.*?)(?=^[A-Za-z]|\Z)', text, re.M | re.S)
allowed = set(re.findall(r'sha256:([0-9a-f]{64})', entry.group(1)))
matches = [p for p in Path(sys.argv[1]).glob('torchreid-1.4.0-*.whl')
           if hashlib.sha256(p.read_bytes()).hexdigest() in allowed]
if not matches:
    raise SystemExit('Missing hash-verified Torchreid 1.4.0 wheel; no environment created')
PY
  mkdir -p -- "$(dirname -- "$runtime")"
  mkdir -- "$runtime"
  "$base_python" -m venv --copies "$runtime"
  "$runtime/bin/python" -m pip --isolated install --no-cache-dir --no-deps \
    --only-binary=:all: --require-hashes --find-links "$wheels" -r "$lock"
  # The inference contract admits exactly the lock's distribution set, without pip.
  "$runtime/bin/python" -m pip uninstall --yes pip
fi
"$runtime/bin/python" "$root/NDNSF-UAV-APP/tracking/person_runtime.py" --lock "$lock"
"$runtime/bin/python" -c 'import torch, torchvision, torchreid, ultralytics, lap; assert torch.version.cuda is None'
"$runtime/bin/python" - "$runtime/share/ndnsf/models" <<'PY'
import hashlib
from pathlib import Path
import sys
from urllib.request import urlopen

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
assets = [
    ('yolo26n.pt',
     'https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt',
     '9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef'),
    ('osnet_x1_0_msmt17.pth',
     'https://huggingface.co/kaiyangzhou/osnet/resolve/'
     'a5c5cc037c24235cda3b21085b93ad77c9616224/'
     'osnet_x1_0_msmt17_combineall_256x128_amsgrad_ep150_stp60_lr0.0015_b64_fb10_softmax_labelsmooth_flip_jitter.pth',
     '48df972f72887b95cf3b43b3a07c3a7d2398381aea0f9cae64a7ef11d512b727'),
]
for name, url, expected in assets:
    target = root / name
    if target.exists():
        digest = hashlib.sha256()
        with target.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise SystemExit('Existing model hash mismatch: ' + str(target))
        continue
    temporary = target.with_suffix(target.suffix + '.download')
    digest = hashlib.sha256()
    with urlopen(url, timeout=60) as response, temporary.open('xb') as stream:
        for block in iter(lambda: response.read(1024 * 1024), b''):
            digest.update(block)
            stream.write(block)
    if digest.hexdigest() != expected:
        raise SystemExit('Downloaded model hash mismatch: ' + str(temporary))
    temporary.rename(target)
    print('Verified model:', target, expected)
PY
echo "Person CPU environment installed: $runtime"
