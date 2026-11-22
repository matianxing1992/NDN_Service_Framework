#!/usr/bin/env bash
set -euo pipefail

profile=${1:?usage: $0 repo|di INSTALLED_PREFIX}
prefix=$(cd "${2:?missing installed prefix}" && pwd -P)
package=libndn-service-framework
bridge="$prefix/include/ndnsf-distributed-repo/RepoSourceProvider.hpp"
case "$profile" in
  repo)
    test ! -e "$bridge"
    test ! -e "$prefix/include/ndnsf-di/api.hpp"
    ;;
  di)
    test -f "$bridge"
    package=ndnsf-distributed-inference
    ;;
  *)
    echo "Unknown SDK profile: $profile" >&2
    exit 2
    ;;
esac

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
test "$(pkg-config --variable=includedir "$package")" = "$prefix/include"
read -r -a flags <<< "$(pkg-config --cflags "$package")"

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
shopt -s nullglob
headers=("$prefix/include/ndnsf-distributed-repo/"*.hpp)
test -f "$prefix/include/ndnsf-distributed-repo/RepoClient.hpp"
for header in "${headers[@]}"; do
  printf '#include <ndnsf-distributed-repo/%s>\n' "${header##*/}" > header.cpp
  "${CXX:-c++}" -std=c++17 -fsyntax-only "${flags[@]}" header.cpp
done
printf 'RepoSDKHeaders PASS profile=%s headers=%s\n' "$profile" "${#headers[@]}"
