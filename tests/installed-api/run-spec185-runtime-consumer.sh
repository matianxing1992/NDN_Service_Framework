#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?usage: $0 <installed-prefix> [output-dir]}
output_dir=${2:-"${TMPDIR:-/tmp}/spec185-runtime-consumer"}
prefix=$(cd "$prefix" && pwd)
source_file=$(cd "$(dirname "$0")" && pwd)/runtime-api-consumer.cpp
repo_root=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
pkg_root="$output_dir/installed"
mkdir -p "$pkg_root"
fixture_dir="$pkg_root/fixture"
mkdir -p "$fixture_dir"

export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig"
unset PKG_CONFIG_PATH
package_libdir=$(pkg-config --variable=libdir ndnsf-distributed-inference)
case "$package_libdir" in
  "$prefix"/*) ;;
  *) echo "PKG_CONFIG_PREFIX_MISMATCH:$package_libdir" >&2; exit 5 ;;
esac
read -r -a cflags <<< "$(pkg-config --cflags ndnsf-distributed-inference)"
read -r -a libs <<< "$(pkg-config --libs ndnsf-distributed-inference)"
for flag in "${cflags[@]}"; do
  case "$flag" in
    *"$repo_root"*) echo "SOURCE_TREE_INCLUDE_LEAK:$flag" >&2; exit 4 ;;
    */ndn-svs|*/ndn-svs/*|*/ndn-svs/build|*/ndn-svs/build/*)
      echo "UNINSTALLED_NDNSF_SVS_INCLUDE_LEAK:$flag" >&2; exit 4 ;;
  esac
done
prefix_include="$prefix/include"
runtime_header="$prefix_include/NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
test -f "$runtime_header"
prefix_include_seen=false
for flag in "${cflags[@]}"; do
  if [[ "$flag" == "-I$prefix_include" ]]; then
    prefix_include_seen=true
  fi
done
if [[ "$prefix_include_seen" != true ]]; then
  echo "INSTALLED_PREFIX_INCLUDE_MISSING:$prefix_include" >&2
  exit 8
fi
library=$(find "$package_libdir" -maxdepth 1 -name 'libndnsf-distributed-inference.so*' -type f | sort | head -n 1)
test -n "$library"

# Build a self-contained operator fixture so this consumer proves a valid
# installed Runtime::open -> user() path rather than only an error symbol.
openssl genpkey -algorithm ED25519 -out "$fixture_dir/requester.pem" >/dev/null 2>&1
openssl genpkey -algorithm ED25519 -out "$fixture_dir/authority.pem" >/dev/null 2>&1
openssl genpkey -algorithm ED25519 -out "$fixture_dir/offer.pem" >/dev/null 2>&1
openssl pkey -in "$fixture_dir/authority.pem" -pubout -out "$fixture_dir/authority-public.pem" >/dev/null 2>&1
openssl pkey -in "$fixture_dir/offer.pem" -pubout -out "$fixture_dir/offer-public.pem" >/dev/null 2>&1
chmod 600 "$fixture_dir/requester.pem" "$fixture_dir/authority.pem" "$fixture_dir/offer.pem"
candidate_digest="sha256:$(printf 'candidate' | sha256sum | awk '{print $1}')"
offer_key_id="sha256:$(openssl pkey -pubin -in "$fixture_dir/offer-public.pem" -outform DER 2>/dev/null | tail -c 32 | sha256sum | awk '{print $1}')"
cat > "$fixture_dir/trust.conf" <<'EOF'
rule
{
  id "Installed Runtime Test Rule"
  for data
  checker
  {
    type hierarchical
    sig-type rsa-sha256
  }
}
trust-anchor
{
  type any
}
EOF
cat > "$fixture_dir/requester.json" <<EOF
{
  "schema":"ndnsf-di-native-requester-v1",
  "core":{"requester_identity":"/user","authority_identity":"/core","group":"/group","trust_schema_file":"trust.conf"},
  "catalog":{"source":{"file":"missing.onnx"}},
  "grant":{"authority_identity":"/aa","authority_service":"/grant","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester.pem","protection_epoch":"epoch-1"},
  "offer_admission":{"policy":{"schema":"spec180-provider-offer-trust-v1","candidateId":"candidate","candidateDigest":"$candidate_digest","trustSchema":"/group/trust","entries":[{"provider":"/provider","service":"/Inference","keyLocatorPrefix":"/provider/KEY/offer","signerKeyId":"$offer_key_id","certificateName":"/provider/KEY/offer/ID-CERT"}]},"public_key_files":{"$offer_key_id":"offer-public.pem"},"candidate_digest":"$candidate_digest"},
  "limits":{"bootstrap_ms":1000,"max_source_bytes":4096,"max_assembled_bytes":8192},
  "request":{"service":"/Inference","task":"task","adapter_composition_digest":"$candidate_digest","task_descriptor_digest":"$candidate_digest","input_layout_digest":"$candidate_digest","security_policy_digest":"$candidate_digest","max_candidates":1,"max_policy_ms":100,"generation_mode":"TOKEN_DIAGNOSTIC","timeout_ms":30000,"ack_timeout_ms":5000}
}
EOF
g++ -std=c++17 -Wall -Wextra -Werror "${cflags[@]}" "$source_file" "${libs[@]}" \
  -I"$prefix_include" \
  -Wl,-rpath,"$package_libdir" -o "$pkg_root/spec185-runtime-consumer"
readelf -d "$pkg_root/spec185-runtime-consumer" > "$pkg_root/readelf"
LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
  ldd "$pkg_root/spec185-runtime-consumer" > "$pkg_root/ldd"
grep -F 'NEEDED' "$pkg_root/readelf" | grep -F 'libndnsf-distributed-inference.so' >/dev/null
loaded=$(awk -v dir="$package_libdir/" \
  '$1 ~ /^libndnsf-distributed-inference\.so/ && $2 == "=>" && index($3, dir) == 1 { print $3; exit }' \
  "$pkg_root/ldd")
test -n "$loaded" && test "$(readlink -f "$library")" = "$(readlink -f "$loaded")"
LD_LIBRARY_PATH="$package_libdir:${LD_LIBRARY_PATH:-}" \
  "$pkg_root/spec185-runtime-consumer" "$fixture_dir/requester.json" > "$pkg_root/run.log" 2>&1
echo "Spec185RuntimeConsumer PASS prefix=$prefix include=$runtime_header library=$loaded"
