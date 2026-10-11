"""Actual pybind value semantics used by the migrated YOLO publisher."""

from py_repoclient.artifact_api import SelectedRepoSet, SelectedRepoTarget


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
