"""Actual pybind value semantics used by the migrated YOLO publisher."""

from py_repoclient.artifact_api import SelectedRepoSet, SelectedRepoTarget
from py_repoclient import ArtifactReference
import pytest


def test_selected_native_targets_are_assigned_as_one_vector():
    target = SelectedRepoTarget()
    target.repo_node = "/repo/one"
    target.provider_identity = "/actual/provider/one"
    target.service_prefix = "/NDNSF/DistributedRepo/Object/v1"
    selected = SelectedRepoSet()
    selected.targets = [target]
    selected.seed_repo_id = target.repo_node
    assert len(selected.targets) == 1
    assert selected.targets[0].repo_node == selected.seed_repo_id


def test_mutating_pybind_vector_copy_does_not_change_native_state():
    selected = SelectedRepoSet()
    selected.targets = []
    selected.targets.append(SelectedRepoTarget())
    assert len(selected.targets) == 0


def test_reference_normalizes_uri_without_changing_legacy_serialization():
    reference = ArtifactReference(
        logical_name="/producer/model", content_digest="0" * 64, size_bytes=1,
        root_manifest_name="/producer/model", publisher_identity="/producer",
        policy_epoch="plain")
    canonical = reference.canonicalized()
    canonical.validate_producer_ownership()
    assert reference.logical_name == "/producer/model"
    assert canonical.logical_name == "/8=producer/8=model"
    assert canonical.publisher_identity == "/8=producer"


def test_reference_rejects_similar_but_foreign_producer_prefix():
    reference = ArtifactReference(
        logical_name="/producer-other/model", content_digest="0" * 64, size_bytes=1,
        root_manifest_name="/producer-other/model", publisher_identity="/producer",
        policy_epoch="plain")
    with pytest.raises(Exception, match="complete publisher namespace"):
        reference.validate_producer_ownership()


def test_raw_file_reference_roundtrips_without_manifest_name():
    reference = ArtifactReference(
        logical_name="/producer/model", content_digest="0" * 64, size_bytes=1,
        root_manifest_name="", publisher_identity="/producer", policy_epoch="plain",
        format_version="raw-file-v1").canonicalized()
    reference.validate_producer_ownership()
    decoded = ArtifactReference.from_json(reference.to_json())
    assert decoded.format_version == "raw-file-v1"
    assert decoded.root_manifest_name == ""
    assert decoded.logical_name == reference.logical_name


def test_raw_file_reference_rejects_invented_manifest():
    with pytest.raises(Exception, match="unpublished root manifest"):
        ArtifactReference(
            logical_name="/producer/model", content_digest="0" * 64, size_bytes=1,
            root_manifest_name="/producer/model/root", publisher_identity="/producer",
            policy_epoch="plain", format_version="raw-file-v1")
