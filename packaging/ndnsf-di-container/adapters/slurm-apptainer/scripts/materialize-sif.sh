#!/bin/sh
set -eu

usage(){ echo "usage: $0 --oci-reference REF@sha256:HEX --sif PATH --record PATH" >&2; exit 2; }
oci=''; sif=''; record=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --oci-reference) oci=$2; shift 2 ;;
    --sif) sif=$2; shift 2 ;;
    --record) record=$2; shift 2 ;;
    *) usage ;;
  esac
done
printf '%s\n' "$oci" | grep -Eq '^[^[:space:]]+@sha256:[a-f0-9]{64}$' || { echo OCI_DIGEST_REQUIRED >&2; exit 4; }
[ -n "$sif" ] && [ -n "$record" ] || usage
mkdir -p "$(dirname "$sif")" "$(dirname "$record")"
version=$(apptainer version)

write_record()
{
  target=$1; recovered=$2
  sif_digest=$(sha256sum "$sif" | cut -d' ' -f1)
  python3 - "$target" "$oci" "$sif" "$sif_digest" "$version" "$recovered" <<'PY'
import hashlib,json,sys
path,oci,sif,sif_digest,version,recovered=sys.argv[1:]
body={'schemaVersion':'ndnsf-sif-materialization-v2','ociReference':oci,
      'ociDigest':'sha256:'+oci.rsplit('@sha256:',1)[1],'sifPath':sif,
      'sifSha256':'sha256:'+sif_digest,'apptainerVersion':version,
      'verified':True,'recoveredAfterPartialPromotion':recovered=='true'}
body['recordDigest']='sha256:'+hashlib.sha256(json.dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
with open(path,'x',encoding='utf-8') as stream:
 json.dump(body,stream,indent=2,sort_keys=True);stream.write('\n')
PY
}

verify_existing()
{
  python3 - "$record" "$oci" "$sif" <<'PY'
import hashlib,json,sys
record,oci,sif=sys.argv[1:];value=json.load(open(record,encoding='utf-8'))
if value.get('ociReference')!=oci or value.get('sifPath')!=sif:raise SystemExit('MATERIALIZATION_EXISTING_IDENTITY_MISMATCH')
digest=hashlib.sha256()
with open(sif,'rb') as stream:
 for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
actual='sha256:'+digest.hexdigest()
if value.get('sifSha256')!=actual:raise SystemExit('MATERIALIZATION_EXISTING_SIF_TAMPERED')
body=dict(value);record_digest=body.pop('recordDigest',None)
expected='sha256:'+hashlib.sha256(__import__('json').dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
if record_digest!=expected:raise SystemExit('MATERIALIZATION_EXISTING_RECORD_TAMPERED')
PY
}

if [ -f "$sif" ] && [ -f "$record" ]; then
  verify_existing
  echo "MATERIALIZATION_EXISTING_VERIFIED sif=$sif"
  exit 0
fi
if [ -f "$sif" ] && [ ! -e "$record" ]; then
  write_record "$record" true
  verify_existing
  echo "MATERIALIZATION_RECORD_RECOVERED sif=$sif"
  exit 0
fi
if [ -e "$record" ] && [ ! -f "$sif" ]; then
  echo MATERIALIZATION_RECORD_WITHOUT_SIF >&2
  exit 4
fi

partial="$sif.partial"
record_partial="$record.partial"
rm -f "$partial" "$record_partial"
complete=false
cleanup(){ [ "$complete" = true ] || rm -f "$partial" "$record_partial"; }
trap cleanup EXIT INT TERM
apptainer build "$partial" "docker://$oci"
[ -s "$partial" ] || { echo MATERIALIZATION_EMPTY_SIF >&2; exit 4; }
mv "$partial" "$sif"
write_record "$record_partial" false
mv "$record_partial" "$record"
complete=true
trap - EXIT INT TERM
verify_existing
echo "MATERIALIZATION_PASS sif=$sif"
