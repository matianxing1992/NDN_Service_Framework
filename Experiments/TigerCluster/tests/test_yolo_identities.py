"""Filesystem identity isolation, not certificate or crypto qualification."""
import os
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import identities


def role_homes(tmp_path):
    homes = {role: tmp_path / role for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")}
    for home in homes.values():
        (home / ".ndn").mkdir(parents=True)
        # This is only an inode/layout fixture, deliberately not a usable PIB.
        (home / ".ndn/pib.db").write_bytes(b"layout-test-only")
        (home / ".ndn/ndnsec-key-file").mkdir()
        (home / ".ndn/ndnsec-key-file/fake.privkey").write_bytes(b"not-a-private-key")
    return homes


def test_four_roles_have_distinct_home_and_pib(tmp_path):
    homes = role_homes(tmp_path)
    assert identities.validate_role_homes(homes) == homes


def test_two_roles_cannot_share_pib_through_hardlink(tmp_path):
    homes = role_homes(tmp_path)
    target = homes["Merge"] / ".ndn/pib.db"
    target.unlink()
    os.link(homes["BackboneNeck"] / ".ndn/pib.db", target)
    with pytest.raises(ValueError, match="SHARED_PIB"):
        identities.validate_role_homes(homes)


@pytest.mark.parametrize("kind", ["symlink-home", "symlink-pib", "missing-pib", "fifo-pib", "nested-home"])
def test_prepared_layout_rejects_aliases_and_non_pib_paths(tmp_path, kind):
    homes = role_homes(tmp_path)
    merge = homes["Merge"]
    if kind == "symlink-home":
        saved = tmp_path / "saved"
        merge.rename(saved)
        merge.symlink_to(saved, target_is_directory=True)
    elif kind == "nested-home":
        nested = homes["BackboneNeck"] / "Merge"
        merge.rename(nested)
        homes["Merge"] = nested
    else:
        pib = merge / ".ndn/pib.db"
        pib.unlink()
        if kind == "symlink-pib":
            pib.symlink_to(homes["BackboneNeck"] / ".ndn/pib.db")
        elif kind == "fifo-pib":
            os.mkfifo(pib)
    with pytest.raises(ValueError, match="ROLE_|SHARED_HOME"):
        identities.validate_role_homes(homes)


@pytest.mark.parametrize("homes", [{}, {"../bad": "/private/bad"}, {"user": "user"},
                                     {"user": None}, {"user": "/private/../user"}])
def test_invalid_role_home_contract_is_rejected(homes):
    with pytest.raises(ValueError, match="ROLE_"):
        identities.validate_role_homes(homes)


def test_private_key_hardlink_is_not_role_isolation(tmp_path):
    homes = role_homes(tmp_path)
    key = homes["Merge"] / ".ndn/ndnsec-key-file/fake.privkey"
    key.unlink()
    os.link(homes["BackboneNeck"] / ".ndn/ndnsec-key-file/fake.privkey", key)
    with pytest.raises(ValueError, match="SHARED_PRIVATE_KEY"):
        identities.validate_role_homes(homes)


@pytest.mark.parametrize("kind", ["missing", "symlink-key", "symlink-directory"])
def test_tpm_must_be_local_to_role_and_contain_private_regular_files(tmp_path, kind):
    homes = role_homes(tmp_path)
    tpm = homes["Merge"] / ".ndn/ndnsec-key-file"
    key = tpm / "fake.privkey"
    if kind == "symlink-directory":
        saved = homes["Merge"] / "saved-tpm"
        tpm.rename(saved)
        tpm.symlink_to(saved, target_is_directory=True)
    else:
        key.unlink()
        if kind == "symlink-key":
            key.symlink_to(homes["BackboneNeck"] / ".ndn/ndnsec-key-file/fake.privkey")
    with pytest.raises(ValueError, match="ROLE_TPM|ROLE_PRIVATE_KEY"):
        identities.validate_role_homes(homes)
