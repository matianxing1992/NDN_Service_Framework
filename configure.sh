#!/usr/bin/env bash
# Compatibility shim: package policy and execution live in the stack installer.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
args=(--configure-only)
while (($#)); do
  case "$1" in
    --dry-run) args+=(--plan); shift ;;
    --no-install) args+=(--no-system-packages); shift ;;
    --help|-h)
      echo 'Usage: ./configure.sh [--dry-run|--no-install] [-- WAF_CONFIGURE_OPTIONS]'
      echo 'Compatibility alias for install_ndnsf_stack.sh --configure-only.'
      echo 'Installs missing OS packages and configures only; never builds dependencies or NDNSF.'
      exit 0 ;;
    --) shift; args+=(-- "$@"); break ;;
    *) echo "Unknown option: $1; put Waf options after --" >&2; exit 2 ;;
  esac
done
exec bash "$project_root/install_ndnsf_stack.sh" "${args[@]}"
