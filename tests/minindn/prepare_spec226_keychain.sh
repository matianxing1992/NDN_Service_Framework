#!/usr/bin/env bash
set -euo pipefail
umask 077

# Prepare a fixed-root trust hierarchy with one private PIB/TPM per actor.
run_root=${1:?usage: prepare_spec226_keychain.sh RUN_ROOT_AS_SEEN_BY_MININDN [TRUST_SCHEMA]}
trust_schema_source=${2:-}
case "$run_root" in
  /*) ;;
  *) echo "run root must be absolute: $run_root" >&2; exit 2 ;;
esac
[[ -d "$run_root" && ! -L "$run_root" ]] || {
  echo "run root must be a prepared non-symlink directory: $run_root" >&2
  exit 2
}

ca="$run_root/private/ca"
config="$run_root/private/config"
issued="$ca/issued"
identities_dir="$run_root/private/identities"
mkdir -p "$ca/home" "$ca/config" "$ca/data" "$ca/pib" "$ca/tpm" \
  "$config" "$issued" "$identities_dir"
chmod 755 "$run_root/private" "$ca" "$issued" "$identities_dir"
chmod 700 "$ca/home" "$ca/config" "$ca/data" "$ca/pib" "$ca/tpm"
if [[ -n "$trust_schema_source" ]]; then
  [[ -f "$trust_schema_source" ]] || {
    echo "trust schema is unavailable: $trust_schema_source" >&2
    exit 2
  }
  cp -- "$trust_schema_source" "$config/trust-schema.conf"
fi

identities=(
  /example/uav/controller
  /example/uav/drone/UAV1
  /example/uav/drone/UAV2
  /example/uav/drone/UAV3
  /example/uav/source-adapter/UAV1
  /example/uav/source-adapter/UAV2
  /example/uav/source-adapter/UAV3
  /example/uav/compute
  /example/uav/compute/display
  /example/uav/gs
)
public_trust_installer=${NDNSF_SPEC226_PUBLIC_TRUST_INSTALLER:-spec226-public-trust-installer}
if ! command -v "$public_trust_installer" >/dev/null 2>&1; then
  echo "public trust installer is unavailable: $public_trust_installer" >&2
  exit 1
fi

use_ca_keychain()
{
  export HOME="$ca/home"
  export XDG_CONFIG_HOME="$ca/config"
  export XDG_DATA_HOME="$ca/data"
  export NDN_CLIENT_PIB="pib-sqlite3:$ca/pib"
  export NDN_CLIENT_TPM="tpm-file:$ca/tpm"
  mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
}

use_actor_keychain()
{
  local actor_dir=$1
  export HOME="$actor_dir/home"
  export XDG_CONFIG_HOME="$actor_dir/config"
  export XDG_DATA_HOME="$actor_dir/data"
  export NDN_CLIENT_PIB="pib-sqlite3:$actor_dir/pib"
  export NDN_CLIENT_TPM="tpm-file:$actor_dir/tpm"
  mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
}

use_ca_keychain
ndnsec key-gen -t e /example/uav >"$ca/root-keygen.log" 2>&1
root_cert_name=$(ndnsec get-default -c -i /example/uav)
ndnsec cert-dump -n "$root_cert_name" >"$config/root.cert"
ndnsec cert-dump -n "$root_cert_name" -p >"$ca/root-cert-readable.txt"

for identity in "${identities[@]}"; do
  slug=${identity#/example/uav/}
  slug=${slug//\//-}
  actor_dir="$identities_dir/$slug/keychain"
  mkdir -p "$actor_dir/home" "$actor_dir/config" "$actor_dir/data" \
    "$actor_dir/pib" "$actor_dir/tpm"
  chmod 755 "$identities_dir/$slug" "$actor_dir" "$actor_dir/home" \
    "$actor_dir/config" "$actor_dir/data" "$actor_dir/pib" "$actor_dir/tpm"

  use_actor_keychain "$actor_dir"
  ndnsec key-gen -t r "$identity" >"$issued/$slug-keygen.log" 2>&1
  ndnsec sign-req "$identity" >"$issued/$slug.req"

  use_ca_keychain
  ndnsec cert-gen --sign-id /example/uav --issuer-id root \
    --request "$issued/$slug.req" >"$issued/$slug.cert"

  use_actor_keychain "$actor_dir"
  ndnsec cert-install --key-default "$issued/$slug.cert" \
    >"$issued/$slug-cert-install.log" 2>&1
  cert_name=$(ndnsec get-default -c -i "$identity")
  case "$cert_name" in
    "$identity"/KEY/*/root/*) ;;
    *) echo "unexpected non-root-signed default for $identity: $cert_name" >&2; exit 1 ;;
  esac
  ndnsec cert-dump -n "$cert_name" -p >"$issued/$slug-cert-readable.txt"
  ndnsec list >"$issued/$slug-pib-identities.txt"
  key_count=$(find "$actor_dir/tpm" -type f -printf . | wc -c)
  if [[ "$key_count" -ne 1 ]]; then
    echo "actor keychain must contain exactly one private key: $identity" >&2
    exit 1
  fi
  printf 'SPEC226_ACTOR_CREDENTIAL_READY identity=%s privateKeys=%s\n' \
    "$identity" "$key_count"
done

# Distribute only public certificates so each actor can resolve and validate
# its peers without sharing any actor's private signing key.
for identity in "${identities[@]}"; do
  slug=${identity#/example/uav/}
  slug=${slug//\//-}
  actor_dir="$identities_dir/$slug/keychain"
  use_actor_keychain "$actor_dir"
  public_certificates=("$config/root.cert")
  for peer_identity in "${identities[@]}"; do
    peer_slug=${peer_identity#/example/uav/}
    peer_slug=${peer_slug//\//-}
    public_certificates+=("$issued/$peer_slug.cert")
  done
  "$public_trust_installer" "$actor_dir/pib" "tpm-file:$actor_dir/tpm" \
    "${public_certificates[@]}" >"$issued/$slug-public-trust-install.log" 2>&1

  default_identity=$(ndnsec get-default)
  if [[ "$default_identity" != "$identity" ]]; then
    echo "public certificate import changed actor default: $identity -> $default_identity" >&2
    exit 1
  fi
  printf 'SPEC226_PUBLIC_TRUST_BUNDLE_READY identity=%s trustedIdentities=%s privateKeys=1\n' \
    "$identity" "$(( ${#identities[@]} + 1 ))"
done

find "$identities_dir" -type d -exec chmod 755 {} +
find "$identities_dir" -type f -exec chmod 600 {} +
chmod 700 "$ca/home" "$ca/config" "$ca/data" "$ca/pib" "$ca/tpm"
chmod 755 "$ca" "$issued"
find "$issued" -type f -exec chmod 644 {} +
find "$config" -type d -exec chmod 755 {} +
find "$config" -type f -exec chmod 644 {} +
printf 'ROOT_CERT=%s\nACTOR_CREDENTIAL_HOMES=%s\n' \
  "$root_cert_name" "$(( ${#identities[@]} ))" >"$ca/locator-manifest.txt"
sha256sum "$config/root.cert" >"$ca/root-cert.sha256"
