# NDNSF-UAV Portable Release Packaging

This directory contains the packaging helpers for releasing NDNSF-UAV as a
binary application instead of a source checkout. The release directory should
contain only the final downloadable artifacts:

```text
RELEASE/
  NDNSF-UAV-ubuntu20-x86_64-<version>.tar.gz
  NDNSF-UAV-nixos-x86_64-<version>.closure.gz
  NDNSF-UAV-nixos-aarch64-<version>.closure.gz
```

The Ubuntu tarball is for Ubuntu 20.04 x86_64 style deployments. The two Nix
closures are for NixOS-style deployment on x86_64 and aarch64. NFD is not
bundled; each target machine should run its own NFD and configure faces,
routes, keychain state, and certificates normally.

## Build the Ubuntu x86_64 Tarball

Build the UAV executables first:

```bash
./waf build -j$(nproc) --targets=App_ServiceController,UavDroneApp,UavGroundStationApp
```

Create the release under `RELEASE/`:

```bash
NDNSF_UAV_RELEASE_VERSION=local-test \
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 \
  packaging/uav-release/create-portable-release.sh RELEASE
```

On the Ubuntu 20.04 x86_64 development host this creates:

```text
RELEASE/NDNSF-UAV-ubuntu20-x86_64-local-test.tar.gz
```

The tarball includes:

```text
bin/       controller, ground-station, and drone binaries plus wrappers
lib/       private runtime libraries discovered from ldd
config/    editable runtime configs, policies, and trust schema
certs/     empty deployment certificate directory
videos/    sample camera input video
scripts/   dependency and deployment preflight helpers
```

Before starting a role on a target machine, run
`scripts/ndnsf-uav-preflight`. It checks NFD reachability, config paths, trust
schema, identity certificates, and stale local certificate choices. For
multi-machine deployments, pass `--expected-cert IDENTITY=FILE` on the
Controller machine so it verifies that the certificate used for encrypted
permission delivery matches the certificate installed on the remote node.
The release wrappers can run the same preflight automatically when
`NDNSF_UAV_PREFLIGHT=1` is set. Extra preflight-only arguments, such as
`--expected-cert`, can be passed through `NDNSF_UAV_PREFLIGHT_ARGS`.

Drone configs default to `video-source auto`: the drone selects the first local
V4L2 capture device and falls back to `videos/drone.mp4` when no usable camera is
present. Set `video-source /dev/videoX` or a file path to force a source.
The default V4L2 capture format is adaptive:
`camera-v4l2-input-format auto`, `camera-v4l2-input-size auto`, and
`camera-v4l2-input-fps 0`. In this mode the drone queries the camera and chooses
a conservative format and size, preferring YUYV 640x480 when available. A frame
rate value of `0` does not force the V4L2 input frame interval and lets the
encoder downsample later. Override these only after checking the target camera with
`ffmpeg -f v4l2 -list_formats all -i /dev/videoX`.

If `ffmpeg` is available on the build host, it is copied into `bin/` and the
wrappers put the release `bin/` directory first in `PATH`. This is required for
USB/V4L2 camera capture because the drone video pipeline invokes `ffmpeg`.

If `patchelf` is installed, the ELF RUNPATH is set to:

```text
$ORIGIN/../lib
```

The wrapper commands also set `LD_LIBRARY_PATH` as a fallback.

## Build Nix Closures

A normal portable tarball is not enough for NixOS because NixOS does not use the
usual `/lib64/ld-linux-*.so.*` dynamic loader path. For NixOS targets, wrap a
portable tarball into a Nix store package and export its closure.

If the build host does not already have Nix, install it first and reopen the
shell, or source the daemon profile:

```bash
sh <(curl -L https://nixos.org/nix/install) --daemon
. /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
```

On the local x86_64 host:

```bash
NDNSF_UAV_RELEASE_VERSION=local-test \
  packaging/uav-release/create-nixos-closure.sh \
  RELEASE/NDNSF-UAV-ubuntu20-x86_64-local-test.tar.gz \
  RELEASE
```

This creates:

```text
RELEASE/NDNSF-UAV-nixos-x86_64-local-test.closure.gz
```

On an aarch64 build host, such as the Debian 12 GCP ARM VM used for Odroid C4
release preparation, build an aarch64 portable tarball and closure there:

```bash
./waf build -j$(nproc) --targets=App_ServiceController,UavDroneApp,UavGroundStationApp
NDNSF_UAV_RELEASE_VERSION=local-test \
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 \
  packaging/uav-release/create-portable-release.sh RELEASE
NDNSF_UAV_RELEASE_VERSION=local-test \
  packaging/uav-release/create-nixos-closure.sh \
  RELEASE/NDNSF-UAV-debian12-aarch64-local-test.tar.gz \
  RELEASE
```

Only copy the final Nix closure back into this repository's `RELEASE/`:

```text
RELEASE/NDNSF-UAV-nixos-aarch64-local-test.closure.gz
```

The closure includes the Nix glibc loader and the Nix runtime paths needed by
the packaged UAV binaries. It also includes Nix `ffmpeg-headless` for USB/V4L2
camera capture without pulling unnecessary GUI media tooling into the closure.
It still does not include NFD, certificates, PX4, jMAVSim, camera devices, or
machine-specific routes.

## Install and Run the Ubuntu Tarball

For Ubuntu 20.04 x86_64 machines:

```bash
tar -xzf NDNSF-UAV-ubuntu20-x86_64-local-test.tar.gz
cd NDNSF-UAV-ubuntu20-x86_64-local-test
./scripts/check-runtime-deps.sh
./bin/ndnsf-uav-controller
./bin/ndnsf-uav-gs
./bin/ndnsf-uav-drone --drone-id A
```

Before real deployment, edit `config/*.conf`, install the required NDN
certificates/safebags, start NFD, and configure routes.

## Install and Run a Nix Closure

Copy the matching closure to the NixOS target and import it:

```bash
gzip -dc NDNSF-UAV-nixos-aarch64-local-test.closure.gz | nix-store --import | tee imported-paths.txt
app=$(grep 'ndnsf-uav-release' imported-paths.txt | tail -1)
echo "$app"
```

The imported store path is read-only. For real deployment, copy the included
configuration templates into a writable directory and point the wrappers there:

```bash
rm -rf ~/NDNSF-UAV-deploy
mkdir -p ~/NDNSF-UAV-deploy
cp -a "$app/config" "$app/certs" "$app/videos" ~/NDNSF-UAV-deploy/
chmod -R u+w ~/NDNSF-UAV-deploy
cd ~/NDNSF-UAV-deploy
export NDNSF_UAV_CONFIG_DIR=$PWD/config
"$app/scripts/check-runtime-deps.sh"
"$app/bin/ndnsf-uav-controller"
"$app/bin/ndnsf-uav-drone" --drone-id A
"$app/bin/ndnsf-uav-gs"
```

The wrappers pass `--runtime-config`, `--app-config`, and `--trust-schema`
from `NDNSF_UAV_CONFIG_DIR` so they do not depend on the shell's current working
directory. They also switch to the deployment root, so relative config values
such as `videos/drone.mp4` resolve against the copied release directory.

To make a wrapper run preflight before starting the app:

```bash
NDNSF_UAV_PREFLIGHT=1 "$app/bin/ndnsf-uav-controller"
NDNSF_UAV_PREFLIGHT=1 \
NDNSF_UAV_PREFLIGHT_ARGS="--expected-cert /example/uav/drone/A=certs/drone-A.cert" \
  "$app/bin/ndnsf-uav-controller"
```

## MiniNDN Release Smoke Test

MiniNDN does not create a clean filesystem without host libraries; it creates
network namespaces and per-node runtime directories. It is still useful for
checking that the packaged apps can run through the NDNSF demo path.

```bash
python3 Experiments/NDNSF_UAV_GUI_Minindn_Release.py \
  --release-dir RELEASE/NDNSF-UAV-ubuntu20-x86_64-local-test \
  --auto-recording-playback-test --no-cli --no-xhost --camera-mode file \
  --output-dir results/uav_recording_playback_release_smoke
```

If `--release-dir` is omitted, the launcher uses the newest unpacked
`RELEASE/NDNSF-UAV-*` directory.
