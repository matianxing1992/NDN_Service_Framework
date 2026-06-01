# NDNSF-UAV Portable Release Packaging

The first supported release target is Ubuntu 20.04 x86_64 built on Ubuntu
20.04. The package is a portable tarball rather than a source installer:

```bash
./waf build
packaging/uav-release/create-portable-release.sh
```

For a partially static UAV build, configure a separate output directory:

```bash
./waf configure -o build-uav-static --enable-static --disable-shared --with-examples
./waf build -o build-uav-static \
  --targets=App_ServiceController,UavDroneApp,UavGroundStationApp
```

This links NDNSF and NDNSF-DistributedRepo from static archives into the three
UAV executables. It does not make the executables fully static unless static
archives for all external dependencies also exist. On the current Ubuntu 20.04
development host, `libndn-cxx.a` is not installed, so the partially static
executables still depend dynamically on `libndn-cxx`, ndn-svs, NDNSD, NAC-ABE,
OpenABE, RELIC, GTK, Boost, OpenSSL, and normal system libraries.

MiniNDN can verify that these partially static executables run inside MiniNDN
nodes:

```bash
NDNSF_UAV_APP_BUILD_DIR="$PWD/build-uav-static/examples" \
  xvfb-run -a sudo -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
    --auto-recording-playback-test --no-cli --no-xhost --camera-mode file \
    --output-dir results/uav_recording_playback_static_smoke
```

MiniNDN does not create a clean filesystem without host libraries; it creates
network namespaces and per-node runtime directories. Use a Docker container,
chroot, VM, or a fresh target machine to prove that a release does not depend
on undeclared host libraries.

To verify an unpacked release directory with MiniNDN, use the release-specific
launcher. It explicitly loads APP binaries from `release-artifacts/<release>/bin`:

```bash
python3 Experiments/NDNSF_UAV_GUI_Minindn_Release.py \
  --release-dir release-artifacts/NDNSF-UAV-ubuntu20-x86_64-local-test \
  --auto-recording-playback-test --no-cli --no-xhost --camera-mode file \
  --output-dir results/uav_recording_playback_release_smoke
```

If `--release-dir` is omitted, the launcher uses the newest unpacked
`release-artifacts/NDNSF-UAV-*` directory.

The tarball includes:

- `bin/App_ServiceController`
- `bin/UavGroundStationApp`
- `bin/UavDroneApp`
- wrapper commands `bin/ndnsf-uav-controller`, `bin/ndnsf-uav-gs`, and
  `bin/ndnsf-uav-drone`
- UAV config templates and demo policies
- private runtime libraries discovered from `ldd`, including `ndn-cxx`,
  NDNSF, ndn-svs, NDNSD, NAC-ABE, OpenABE, and RELIC when present

If `patchelf` is installed on the build host, the executable RUNPATH is patched
to:

```text
$ORIGIN/../lib
```

That makes the apps prefer the colocated `lib/` directory over global
installations. The wrapper commands also set `LD_LIBRARY_PATH` to the same
directory as a fallback.

NFD is not bundled. It is a host daemon with sockets, routes, faces, keychain
state, and system service integration. Each target machine should run its own
compatible NFD and expose the normal local NFD socket.

For sparse Ubuntu 20.04 lab machines, the script can also bundle most
`ldd`-discovered system libraries:

```bash
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 \
  packaging/uav-release/create-portable-release.sh
```

This larger mode is meant for same-OS deployments. It deliberately does not
try to bundle glibc, the dynamic loader, kernel drivers, NFD, certificates,
camera devices, PX4, or jMAVSim.
