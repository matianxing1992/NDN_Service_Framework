#!/bin/bash
# T011 SDK closure check: build a C++ consumer against the installed NDNSF
# artifacts only (no checkout include paths, no build tree), in a directory
# outside this checkout. Covers the DI+Repo and DI-only link closures.
set -euo pipefail

PREFIX="${1:-/usr/local}"
WORK="${NDNSF_SDK_CHECK_DIR:-$(mktemp -d /tmp/ndnsf-sdk-check.XXXXXX)}"
cleanup() {
  if [ -z "${NDNSF_SDK_CHECK_DIR:-}" ]; then rm -rf "$WORK"; fi
}
trap cleanup EXIT

mkdir -p "$WORK"
cat > "$WORK/consumer.cpp" <<'CPP'
#include <NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp>
#include <iostream>

int main()
{
  ndnsf::di::NativeStoredModelPacketIo io;
  io.requireCurrent = [] {};
  io.commit = [] (auto, const std::string&) {};
  io.fetch = [] (const std::vector<ndn::Name>&) {
    return ndnsf::di::NativeStoredModelPacketIo::PacketWires{};
  };
  std::cout << "SDK_CONSUMER_OK" << std::endl;
  return 0;
}
CPP

COMMON=(-I"$PREFIX/include" -L"$PREFIX/lib"
        -lndn-service-framework -lndn-cxx -lnac-abe
        -lboost_system -lboost_chrono -lboost_program_options -lboost_thread
        -lssl -lcrypto -lpthread)

g++ -std=c++17 -o "$WORK/consumer-di-repo" "$WORK/consumer.cpp" \
  "${COMMON[@]}" -lndnsf-distributed-inference -lndnsf-distributed-repo
g++ -std=c++17 -o "$WORK/consumer-di-only" "$WORK/consumer.cpp" \
  "${COMMON[@]}" -lndnsf-distributed-inference
"$WORK/consumer-di-repo"
"$WORK/consumer-di-only"
echo "INSTALLED_SDK_CHECK_PASS prefix=$PREFIX work=$WORK"
