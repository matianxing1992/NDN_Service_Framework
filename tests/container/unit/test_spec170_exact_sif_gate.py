from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
_LIB_INSERTED = str(LIB) not in sys.path
if _LIB_INSERTED:
    sys.path.insert(0, str(LIB))
if str(Path(__file__).parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).parent))

import spec170_allocation_topology as topology
import spec170_sif_build_boundary as build_boundary
from test_spec170_allocation_topology import profile

# This directory contains a generic ``profile.py`` deployment helper.  Do not
# leave it at the front of the process-wide import path: later tests importing
# the standard-library ``profile`` module (e.g. through cProfile/torch) would
# otherwise resolve the deployment helper and fail during collection.
if _LIB_INSERTED:
    sys.path.remove(str(LIB))


class Spec170ExactSifGateTest(unittest.TestCase):
    def test_exact_sif_digest_is_verified(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sif = Path(directory) / "runtime.sif"
            sif.write_bytes(b"immutable-spec170-sif")
            value = profile("d0-cpu")
            value["sifPath"] = str(sif)
            value["sifSha256"] = topology.digest_file(sif)
            self.assertEqual("PASS", topology.validate_exact_sif(value)["status"])

    def test_sif_mutation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sif = Path(directory) / "runtime.sif"
            sif.write_bytes(b"immutable-spec170-sif")
            value = profile("d1-single")
            value["sifPath"] = str(sif)
            value["sifSha256"] = topology.digest_file(sif)
            sif.write_bytes(b"tampered")
            with self.assertRaisesRegex(topology.Spec170TopologyError, "SIF_TAMPERED"):
                topology.validate_exact_sif(value)

    def test_missing_exact_sif_is_not_a_pass(self) -> None:
        value = profile("d2h-hybrid")
        with self.assertRaisesRegex(topology.Spec170TopologyError, "SIF_MISSING"):
            topology.validate_exact_sif(value)


class Spec170SifBuildBoundaryTest(unittest.TestCase):
    @staticmethod
    def valid_definition(base: Path, source: Path) -> str:
        return f"""Bootstrap: localimage
From: {base}
Stage: builder

%files
    {source} /build/source.tar

%post
    export NDNSF_CONTAINER_BUILD=1
    ./waf --targets=di-native-provider
    /opt/venv/bin/pip install ./pythonWrapper
    touch /opt/ndnsf-stage/manifest/container-configure-closure.json
    touch /opt/ndnsf-stage/manifest/container-native-build.json

Bootstrap: localimage
From: {base}
Stage: final

%files from builder
    /opt/ndnsf-stage/bin/di-native-provider /opt/ndnsf-di/current/bin/di-native-provider
    /opt/ndnsf-stage/lib/libndn-service-framework.so.0.1.0 /opt/ndnsf-di/current/lib/libndn-service-framework.so.0.1.0
    /opt/ndnsf-stage/python /opt/venv/lib/python3.10/site-packages
    /opt/ndnsf-stage/manifest/container-native-build.json /opt/ndnsf-di/current/manifest/container-native-build.json

%post
    export NDNSF_REPLACE_STALE_NATIVE=1
    rm -f /opt/ndnsf-di/current/bin/di-native-provider
    rm -f /opt/ndnsf-di/current/lib/libndn-service-framework.so*
    rm -f /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf*.so
    find /opt/venv/lib/python3.10/site-packages/ndnsf -name '_ndnsf*.so'
    sha256sum -c /opt/ndnsf-di/current/manifest/container-native-build.json

%labels
    org.ndnsf.di.build-boundary container-runtime-in-sif
    org.ndnsf.di.native-build-manifest /opt/ndnsf-di/current/manifest/container-native-build.json
"""

    def test_multistage_container_build_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definition = root / "candidate.def"
            source = root / "source.tar"
            source.write_bytes(b"sealed source")
            definition.write_text(
                self.valid_definition(root / "base.sif", source), encoding="utf-8"
            )
            self.assertEqual(
                "PASS", build_boundary.validate_definition(definition)["status"]
            )

    def test_host_built_extension_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            extension = root / "_ndnsf.cpython-310-x86_64-linux-gnu.so"
            extension.write_bytes(b"host binary")
            definition = root / "candidate.def"
            definition.write_text(
                f"Bootstrap: localimage\nFrom: {root / 'base.sif'}\n\n"
                f"%files\n    {extension} /opt/venv/site-packages/ndnsf/_ndnsf.so\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                build_boundary.Spec170BuildBoundaryError,
                "WRONG_BUILD_BOUNDARY",
            ):
                build_boundary.validate_definition(definition)

    def test_missing_old_host_extension_is_still_rejected_as_host_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "deleted-host-build" / "ndnsf"
            definition = root / "r13.def"
            definition.write_text(
                f"Bootstrap: localimage\nFrom: {root / 'base.sif'}\n\n"
                f"%files\n    {missing} "
                "/opt/venv/lib/python3.10/site-packages/ndnsf\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                build_boundary.Spec170BuildBoundaryError,
                "WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT",
            ):
                build_boundary.validate_definition(definition)

    def test_base_stale_artifact_replacement_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definition = root / "candidate.def"
            source = root / "source.tar"
            source.write_bytes(b"sealed source")
            text = self.valid_definition(root / "base.sif", source)
            text = text.replace("    export NDNSF_REPLACE_STALE_NATIVE=1\n", "")
            definition.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                build_boundary.Spec170BuildBoundaryError,
                "WRONG_BUILD_BOUNDARY_STALE_REPLACEMENT_MARKER_MISSING",
            ):
                build_boundary.validate_definition(definition)

    def test_fault_provider_must_cross_builder_boundary_when_declared(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definition = root / "candidate.def"
            source = root / "source.tar"
            source.write_bytes(b"sealed source")
            text = self.valid_definition(root / "base.sif", source)
            text = text.replace(
                "./waf --targets=di-native-provider",
                "./waf --targets=di-native-provider,di-native-fault-provider",
            )
            definition.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                build_boundary.Spec170BuildBoundaryError,
                "WRONG_BUILD_BOUNDARY_FAULT_PROVIDER_TRANSFER_MISSING",
            ):
                build_boundary.validate_definition(definition)

    def test_single_stage_definition_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            definition = Path(directory) / "candidate.def"
            definition.write_text(
                "Bootstrap: localimage\nFrom: /tmp/base.sif\n%post\n true\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                build_boundary.Spec170BuildBoundaryError,
                "MULTISTAGE_REQUIRED",
            ):
                build_boundary.validate_definition(definition)


if __name__ == "__main__":
    unittest.main()
