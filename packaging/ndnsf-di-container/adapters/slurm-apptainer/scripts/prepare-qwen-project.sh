#!/usr/bin/env bash
set -euo pipefail
root="${1:-/project/$USER/ndnsf-di}"
case "$root" in
  /project/*/ndnsf-di) ;;
  *)
    if [[ "${NDNSF_SPEC109_ALLOW_TEST_ROOT:-0}" != 1 ]]; then
      echo "PROJECT_ROOT_INVALID:$root" >&2; exit 2
    fi
    ;;
esac
umask 027
install -d -m 0750 "$root"
for name in src images models cache manifests evidence; do install -d -m 0750 "$root/$name"; done
install -d -m 0750 "$root/models/source" "$root/models/onnx" "$root/models/.partial" "$root/manifests/models"
python3 - "$root" <<'PY'
import json,os,stat,sys
from pathlib import Path
root=Path(sys.argv[1]); rows=[]
for path in sorted([root,*root.iterdir()]):
 s=path.stat(); rows.append({'path':str(path),'uid':s.st_uid,'gid':s.st_gid,'mode':oct(stat.S_IMODE(s.st_mode))})
print(json.dumps({'schemaVersion':'1.0','root':str(root),'entries':rows},sort_keys=True))
PY
