#!/bin/bash
# Spec180 Tiger node-local identity bootstrap (runs inside the sealed SIF).
# Usage:
#   bootstrap-tiger-identities.sh init ROOT_HOME ROOT_IDENTITY   # once
#   bootstrap-tiger-identities.sh add HOME ROOT_CERT ROOT_KEY IDENTITY
# The root key is generated once and imported into every process home so
# all signer certificates validate under one trust chain.
set -euo pipefail

command_name=${1:?}

if [[ $command_name == init ]]; then
  root_home=${2:?}
  root_identity=${3:?}
  export HOME="$root_home"
  mkdir -p "$HOME/.ndn"
  ndnsec key-gen -t r "$root_identity" >/dev/null
  ndnsec cert-dump -i "$root_identity" >"$HOME/.ndn/root.cert"
  root_key_name=$(ndnsec list -k 2>/dev/null \
    | grep "${root_identity}/KEY/" | awk '{print $NF}')
  ndnsec-export -P 123456 -o "$HOME/.ndn/root.key" -k \
    "$root_key_name" >/dev/null 2>&1 || true
  ndnsec set-default -n "$root_identity" >/dev/null 2>&1 || true
  echo "ROOT_INITIALIZED"
  exit 0
fi

if [[ $command_name == add ]]; then
  home_dir=${2:?}
  root_cert=${3:?}
  root_key=${4:?}
  identity=${5:?}
  export HOME="$home_dir"
  mkdir -p "$HOME/.ndn"
  if ! ndnsec get-default -i "$identity" >/dev/null 2>&1; then
    ndnsec-import -P 123456 "$root_key" >/dev/null 2>&1 || true
    ndnsec cert-install -f "$root_cert" >/dev/null
    request="$home_dir/.ndn/identity-request.txt"
    certificate="$home_dir/.ndn/identity-cert.txt"
    ndnsec key-gen -t r "$identity" >"$request"
    ndnsec cert-gen -s "/example" -i ROOT "$request" >"$certificate"
    ndnsec cert-install -f "$certificate" >/dev/null
    rm -f "$request" "$certificate"
  fi
  ndnsec set-default -n "$identity" >/dev/null 2>&1 || true
  ndnsec get-default -k -i "$identity" >/dev/null 2>&1 || {
    key_name=$(ndnsec list -k 2>/dev/null | awk -v id="$identity" \
      '$0 ~ id {print $NF}' | head -1)
    if [[ -n ${key_name:-} ]]; then
      ndnsec set-default -k -n "$key_name" >/dev/null 2>&1 || true
    fi
  }
  echo "BOOTSTRAPPED $identity"
  exit 0
fi

echo "usage: bootstrap-tiger-identities.sh init|add ..." >&2
exit 2
