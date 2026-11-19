"""Owned credentials for Python tests that construct a native ServiceUser."""

import os
from pathlib import Path
import subprocess


def provision_native_identities(workspace, controller, user):
    anchor = workspace / "controller.cert"
    environment = {
        "NDN_CLIENT_PIB": "pib-sqlite3:" + str(workspace / "pib"),
        "NDN_CLIENT_TPM": "tpm-file:" + str(workspace / "tpm"),
        "NDN_CLIENT_TRANSPORT": "unix://" + str(workspace / "isolated-nfd.sock"),
        "NDNSF_CONTROLLER_CERT_FILE": str(anchor),
    }
    setup = {**os.environ, **environment}
    setup.pop("LD_PRELOAD", None)
    certificate = subprocess.run(
        ["ndnsec", "key-gen", "-t", "r", controller],
        env=setup, capture_output=True, check=True, timeout=15,
    ).stdout
    anchor.write_bytes(certificate)
    subprocess.run(
        ["ndnsec", "key-gen", "-t", "r", user],
        env=setup, capture_output=True, check=True, timeout=15,
    )

    fixture_root = Path(__file__).resolve().parents[1] / "fixtures"
    rule = (fixture_root / "config-trust.conf").read_text().split("trust-anchor", 1)[0]
    schema = workspace / "trust-schema.conf"
    schema.write_text(
        rule + 'trust-anchor { type file file-name "controller.cert" }\n',
        encoding="utf-8",
    )
    return environment, schema
