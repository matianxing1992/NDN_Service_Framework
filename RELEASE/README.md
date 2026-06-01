# NDNSF-UAV Release Files

This directory intentionally contains only the final downloadable release files:

```text
NDNSF-UAV-ubuntu20-x86_64-local-test.tar.gz
NDNSF-UAV-nixos-x86_64-local-test.closure.gz
NDNSF-UAV-nixos-aarch64-local-test.closure.gz
```

Use the Ubuntu tarball on Ubuntu 20.04 x86_64 machines. Use the Nix closures on
NixOS-style targets, including the Odroid C4 aarch64 target.

NFD is not included in any release file. Each target machine still needs a
running NFD, configured routes/faces, and installed NDN certificates.

## Ubuntu 20.04 x86_64

Extract the tarball:

```bash
tar -xzf NDNSF-UAV-ubuntu20-x86_64-local-test.tar.gz
cd NDNSF-UAV-ubuntu20-x86_64-local-test
```

Check dependencies:

```bash
./scripts/check-runtime-deps.sh
```

Edit the deployment configuration before real use:

```text
config/uav_runtime.conf      shared namespace/controller/trust settings
config/ground-station.conf   ground station identity, target drones, UI defaults
config/drone-A.conf          Drone A identity, camera, MAVLink, repo settings
config/drone-B.conf          Drone B identity, camera, MAVLink, repo settings
config/uav_demo.policies     demo permission policy
config/trust-schema.conf     trust schema used by controller/apps
certs/                       place deployment certs or safebags here if desired
videos/                      sample video input; replace with real camera setup later
```

Drone configs use `video-source auto` by default. In this mode the drone picks
the first local V4L2 capture device such as `/dev/video1`; if no capture device
is available, it falls back to `videos/drone.mp4`. Set `video-source /dev/videoX`
or a file path in the drone config to force a specific source.
For USB UVC cameras, the default capture request is adaptive:
`camera-v4l2-input-format auto`, `camera-v4l2-input-size auto`, and
`camera-v4l2-input-fps 0`. In this mode the drone queries the camera and chooses
a conservative format and size, preferring YUYV 640x480 when available. A frame
rate value of `0` does not force the V4L2 input frame interval and lets the
encoder downsample later. Change these config keys only after checking the
camera's supported formats with
`ffmpeg -f v4l2 -list_formats all -i /dev/videoX`.

Run the apps:

```bash
./bin/ndnsf-uav-controller
./bin/ndnsf-uav-gs --app-config config/ground-station.conf
./bin/ndnsf-uav-drone --drone-id A
./bin/ndnsf-uav-drone --app-config config/drone-B.conf --drone-id B
```

The wrappers pass `--runtime-config`, `--app-config`, and `--trust-schema`
automatically from `./config` when launched from the extracted release
directory, then switch to that deployment root so relative paths such as
`videos/drone.mp4` still work. You can point them to another writable config
directory:

```bash
export NDNSF_UAV_CONFIG_DIR=/path/to/config
./bin/ndnsf-uav-drone --drone-id A
```

## NixOS x86_64 or aarch64

Import the matching closure:

```bash
gzip -dc NDNSF-UAV-nixos-aarch64-local-test.closure.gz | \
  sudo nix-store --option require-sigs false --import | tee imported-paths.txt
app=$(grep 'ndnsf-uav-release' imported-paths.txt | tail -1)
echo "$app"
```

Use `NDNSF-UAV-nixos-x86_64-local-test.closure.gz` instead on x86_64 NixOS.
`require-sigs false` is needed because these closures are local release
artifacts, not paths signed by a public Nix binary cache.

The Nix store path is read-only, so copy the bundled templates to a writable
deployment directory:

```bash
rm -rf ~/NDNSF-UAV-deploy
mkdir -p ~/NDNSF-UAV-deploy
cp -a "$app/config" "$app/certs" "$app/videos" ~/NDNSF-UAV-deploy/
chmod -R u+w ~/NDNSF-UAV-deploy
cd ~/NDNSF-UAV-deploy
export NDNSF_UAV_CONFIG_DIR=$PWD/config
```

Edit the copied `config/*.conf`, install certificates/key material, start NFD,
and then run preflight checks before starting each role:

```bash
"$app/scripts/ndnsf-uav-preflight" --role controller --policy-file config/uav_demo.policies
"$app/scripts/ndnsf-uav-preflight" --role ground-station --app-config config/ground-station.conf
"$app/scripts/ndnsf-uav-preflight" --role drone --app-config config/drone-A.conf
```

The wrapper commands can run the same checks automatically before startup:

```bash
NDNSF_UAV_PREFLIGHT=1 "$app/bin/ndnsf-uav-controller"
NDNSF_UAV_PREFLIGHT=1 "$app/bin/ndnsf-uav-drone" --drone-id A
NDNSF_UAV_PREFLIGHT=1 "$app/bin/ndnsf-uav-gs" --app-config config/ground-station.conf
```

Pass deployment-specific preflight options through `NDNSF_UAV_PREFLIGHT_ARGS`.
For example, on the controller:

```bash
NDNSF_UAV_PREFLIGHT=1 \
NDNSF_UAV_PREFLIGHT_ARGS="--expected-cert /example/uav/drone/A=certs/drone-A.cert" \
  "$app/bin/ndnsf-uav-controller"
```

For multi-machine deployment, export each node's public certificate and make
the controller compare it before startup. This catches the common failure where
the controller still has an old certificate for a drone and encrypts provider
permissions to the wrong key:

```bash
ndnsec cert-dump -i /example/uav/drone/A > certs/drone-A.cert
"$app/scripts/ndnsf-uav-preflight" --role controller \
  --expected-cert /example/uav/drone/A=certs/drone-A.cert
```

Then start the apps:

```bash
"$app/scripts/check-runtime-deps.sh"
"$app/bin/ndnsf-uav-controller"
"$app/bin/ndnsf-uav-drone" --drone-id A
"$app/bin/ndnsf-uav-gs" --app-config config/ground-station.conf
```

## Notes

- The release bundles NDNSF and the runtime libraries needed by the UAV apps.
- The release includes `ffmpeg` for USB/V4L2 camera capture when it was
  available on the build host. Nix closures use `ffmpeg-headless` for the same
  capture path.
- NFD, PX4/jMAVSim, camera devices, routes, and certificates remain deployment
  responsibilities.
- For a real multi-machine deployment, keep the namespace, identities,
  controller prefix, policies, and trust schema consistent across all nodes.
