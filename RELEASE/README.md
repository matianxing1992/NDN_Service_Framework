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

Run the apps:

```bash
./bin/ndnsf-uav-controller
./bin/ndnsf-uav-gs
./bin/ndnsf-uav-drone --drone-id A
./bin/ndnsf-uav-drone --app-config config/drone-B.conf --drone-id B
```

The wrappers use `./config` automatically when launched from the extracted
release directory. You can point them to another writable config directory:

```bash
export NDNSF_UAV_CONFIG_DIR=/path/to/config
./bin/ndnsf-uav-drone --drone-id A
```

## NixOS x86_64 or aarch64

Import the matching closure:

```bash
gzip -dc NDNSF-UAV-nixos-aarch64-local-test.closure.gz | nix-store --import | tee imported-paths.txt
app=$(grep 'ndnsf-uav-release' imported-paths.txt | tail -1)
echo "$app"
```

Use `NDNSF-UAV-nixos-x86_64-local-test.closure.gz` instead on x86_64 NixOS.

The Nix store path is read-only, so copy the bundled templates to a writable
deployment directory:

```bash
rm -rf ~/NDNSF-UAV-deploy
mkdir -p ~/NDNSF-UAV-deploy
cp -a "$app/config" "$app/certs" "$app/videos" ~/NDNSF-UAV-deploy/
cd ~/NDNSF-UAV-deploy
export NDNSF_UAV_CONFIG_DIR=$PWD/config
```

Edit the copied `config/*.conf`, install certificates/key material, start NFD,
and then run:

```bash
"$app/scripts/check-runtime-deps.sh"
"$app/bin/ndnsf-uav-controller"
"$app/bin/ndnsf-uav-drone" --drone-id A
"$app/bin/ndnsf-uav-gs"
```

## Notes

- The release bundles NDNSF and the runtime libraries needed by the UAV apps.
- NFD, PX4/jMAVSim, camera devices, routes, and certificates remain deployment
  responsibilities.
- For a real multi-machine deployment, keep the namespace, identities,
  controller prefix, policies, and trust schema consistent across all nodes.
