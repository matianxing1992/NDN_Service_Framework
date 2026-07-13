#!/bin/sh
set -eu
usage(){ echo "usage: $0 --oci-reference REF@sha256:HEX --sif NEW --record NEW" >&2; exit 2; }
oci=''; sif=''; record=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --oci-reference) oci=$2; shift 2;; --sif) sif=$2; shift 2;; --record) record=$2; shift 2;; *) usage;;
  esac
done
printf '%s\n' "$oci" | grep -Eq '^[^[:space:]]+@sha256:[a-f0-9]{64}$' || { echo OCI_DIGEST_REQUIRED >&2; exit 4; }
if [ -e "$sif" ] || [ -e "$record" ]; then echo MATERIALIZATION_OUTPUT_EXISTS >&2; exit 2; fi
mkdir -p "$(dirname "$sif")" "$(dirname "$record")"
tmp="$sif.tmp.$$"; trap 'rm -f "$tmp"' EXIT INT TERM
version=$(apptainer version)
apptainer build "$tmp" "docker://$oci"
mv "$tmp" "$sif"; trap - EXIT INT TERM
sif_digest=$(sha256sum "$sif" | cut -d' ' -f1); oci_digest=${oci##*@}
python3 - "$record" "$oci" "$oci_digest" "$sif" "$sif_digest" "$version" <<'PY'
import json,sys
path,oci,oci_digest,sif,sif_digest,version=sys.argv[1:]
value={'schema':'ndnsf-sif-materialization-v1','ociReference':oci,'ociDigest':oci_digest,'sifPath':sif,'sifSha256':'sha256:'+sif_digest,'apptainerVersion':version,'verified':True}
open(path,'x').write(json.dumps(value,indent=2,sort_keys=True)+'\n')
PY
