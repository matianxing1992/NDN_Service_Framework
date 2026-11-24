from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

# Two cases assert on the local-only Spec 182 contract documents, which live in the git-ignored `specs/`
# tree and are absent from this checkout. The rest check product behaviour and must keep running.


def test_native_binding_has_no_python_strategy_or_legacy_import() -> None:
    source = (ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp").read_text(
        encoding="utf-8")
    # A Python callback is allowed only at the terminal observer facade; model
    # selection, planning, and execution strategy remain native-owned.
    assert 'py::function callback' in source
    assert 'NativeInferenceHandle::observe' not in source
    assert "placement.py" not in source
    assert "runtime_v1" not in source
    assert "subprocess" not in source


def test_setup_compiles_binding_and_links_the_shared_di_library() -> None:
    setup = (ROOT / "pythonWrapper/setup.py").read_text(encoding="utf-8")
    assert '"src/ndnsf/di_bindings.cpp"' in setup
    assert '"ndnsf-distributed-inference"' in setup
    assert "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp" not in setup
    assert "NativeGrantVerifier.cpp" not in setup


def test_maintained_yolo_native_route_is_explicit_and_fail_closed() -> None:
    user = (ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py").read_text(
        encoding="utf-8")
    assert '--native-requester-config", required=True' in user
    assert "api.Runtime.open(config)" in user
    assert "runtime.user()" in user and "user.prepare()" in user
    assert "native YOLO requester model identity does not match package" in user
    for retired in ("APPClient", "configure_automatic_planning", "request_task",
                    "--offline-oracle", "--native-tensor-input"):
        assert retired not in user
    assert "runtime.close()" in user and "runtime.drain(timeout_s=6)" in user


def test_maintained_yolo_native_route_uses_repository_reference() -> None:
    user = (ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py").read_text(
        encoding="utf-8")
    native_branch = user[user.index("def _load_yolo_native_payload"):user.index("def main()")]
    assert "user.publish_input" in native_branch
    assert "api.Input.repository" in native_branch
    assert "user.request" in native_branch
    assert native_branch.index("user.publish_input") < native_branch.index("user.request")
    assert "request_native_reference" not in native_branch
    assert "fetch/decrypt" in native_branch
