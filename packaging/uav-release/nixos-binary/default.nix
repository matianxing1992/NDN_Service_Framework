{ pkgs ? import <nixpkgs> {}
, releaseTarball ? null
, releaseName ? "ndnsf-uav-release"
}:

assert releaseTarball != null;

pkgs.stdenvNoCC.mkDerivation {
  pname = releaseName;
  version = "nixos-${pkgs.stdenv.hostPlatform.system}";
  src = releaseTarball;

  nativeBuildInputs = [
    pkgs.file
    pkgs.findutils
    pkgs.gnutar
    pkgs.gzip
    pkgs.patchelf
  ];

  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    mkdir -p "$out"
    tar -xzf "$src" --strip-components=1 -C "$out"
    chmod -R u+w "$out"

    interp="${pkgs.stdenv.cc.bintools.dynamicLinker}"
    rpath="$out/lib:${pkgs.glibc}/lib:${pkgs.stdenv.cc.cc.lib}/lib"

    for f in $(find "$out/bin" "$out/lib" -type f 2>/dev/null); do
      if file -b "$f" | grep -q ELF; then
        if patchelf --print-interpreter "$f" >/dev/null 2>&1; then
          patchelf --set-interpreter "$interp" "$f" || true
        fi
        patchelf --set-rpath "$rpath" "$f" || true
      fi
    done

    cat > "$out/README_NIXOS.md" <<README
# NDNSF-UAV NixOS closure

This is a Nix-store packaged NDNSF-UAV release for ${pkgs.stdenv.hostPlatform.system} NixOS.
The ELF interpreter and RPATH are patched to use the Nix glibc loader and this
package's bundled lib directory. NFD is still external and must be running on
the target node.

Deployment configuration is intentionally distributed separately. Extract the
matching deployment-config tarball into a writable directory and either run the
wrapper commands from that directory or set NDNSF_UAV_CONFIG_DIR.
README
  '';
}
