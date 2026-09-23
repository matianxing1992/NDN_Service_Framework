#!/usr/bin/env bash
# Install OS prerequisites only, then delegate configuration to root Waf.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:${PATH}"
mode=install
waf_args=()
while (($#)); do
  case "$1" in
    --dry-run) mode=plan; shift ;;
    --no-install) mode=check; shift ;;
    --help|-h)
      echo 'Usage: ./configure.sh [--dry-run|--no-install] [-- WAF_CONFIGURE_OPTIONS]'
      echo 'Default: install missing Debian/Ubuntu OS prerequisites, then waf configure.'
      echo 'Never builds NDN libraries, downloads SDKs, builds NDNSF, or runs waf install.'
      exit 0 ;;
    --) shift; waf_args=("$@"); break ;;
    *) echo "Unknown option: $1; put Waf options after --" >&2; exit 2 ;;
  esac
done
packages=(build-essential binutils pkg-config cmake ninja-build python3
  python3-dev python3-pip python3-setuptools python3-wheel
  libboost-all-dev libssl-dev libsqlite3-dev libprotobuf-dev protobuf-compiler
  libgtkmm-3.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
  libffi-dev libpcre3-dev zlib1g-dev)
if [[ "$mode" == plan ]]; then
  printf 'OS prerequisites (missing packages only): %s\n' "${packages[*]}"
  printf 'Then: python3 %q configure' "$project_root/waf"
  printf ' %q' "${waf_args[@]}"
  printf '\nNDN libraries and versioned SDKs must already be installed.\n'
  exit 0
fi
if [[ "$mode" == install ]]; then
  if ! command -v apt-get >/dev/null || ! command -v dpkg-query >/dev/null; then
    echo 'Automatic installation supports Debian/Ubuntu only; use --no-install elsewhere.' >&2
    exit 2
  fi
  missing=()
  for package in "${packages[@]}"; do
    state="$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)"
    [[ "$state" == 'install ok installed' ]] || missing+=("$package")
  done
  if ((${#missing[@]})); then
    privilege=()
    if ((EUID != 0)); then
      command -v sudo >/dev/null || { echo 'sudo is required to install packages' >&2; exit 2; }
      privilege=(sudo)
    fi
    "${privilege[@]}" apt-get update
    "${privilege[@]}" apt-get install --no-install-recommends -y "${missing[@]}"
  fi
fi
cd -- "$project_root"
# Never sudo Waf; system installation above is the only privileged operation.
exec /usr/bin/python3 "$project_root/waf" configure "${waf_args[@]}"
